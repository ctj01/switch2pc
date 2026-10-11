#include <inttypes.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "common/hos_log.h"
#include "cpu_internal.h"
#include "patch/patch.h"

#define MOD "trap"

/*
 * HOS_TRAP_STATS=1: count every trap by kind and print the totals and rates
 * at exit. This is what decides whether trampolines (#5, #23) are worth it.
 */
static int g_stats;
static uint64_t g_stats_start_ns;
static _Atomic uint64_t g_svc_count[HOS_SVC_MAX + 1];
static _Atomic uint64_t g_tick_count;

static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void print_stats(void) {
    double secs = (double)(mono_ns() - g_stats_start_ns) / 1e9;
    uint64_t ticks = atomic_load(&g_tick_count), svcs = 0;
    for (int i = 0; i <= HOS_SVC_MAX; i++) {
        svcs += atomic_load(&g_svc_count[i]);
    }
    HOS_LOGI(MOD,
             "trap stats over %.1f s: %" PRIu64 " counter reads (%.0f/s), %" PRIu64
             " svcs (%.0f/s)",
             secs, ticks, (double)ticks / secs, svcs, (double)svcs / secs);
    for (int i = 0; i <= HOS_SVC_MAX; i++) {
        uint64_t n = atomic_load(&g_svc_count[i]);
        if (n * 100 >= svcs && n) { /* the ones that make up at least 1 percent */
            HOS_LOGI(MOD, "  svc %#04x: %" PRIu64 " (%.0f/s)", i, n, (double)n / secs);
        }
    }
}

static const char *sig_name(int sig) {
    switch (sig) {
    case SIGTRAP:
        return "SIGTRAP";
    case SIGSEGV:
        return "SIGSEGV";
    case SIGBUS:
        return "SIGBUS";
    case SIGILL:
        return "SIGILL";
    case SIGFPE:
        return "SIGFPE";
    default:
        return "signal";
    }
}

/* The module whose image contains `addr`, or NULL. */
static const hos_cpu_module *module_at(uint64_t addr) {
    for (uint32_t i = 0; i < hos_params.num_modules; i++) {
        const hos_cpu_module *m = &hos_params.modules[i];
        if (addr >= m->image_base && addr < m->image_base + m->image_size) {
            return m;
        }
    }
    return NULL;
}

static int read_text_insn(uint64_t pc, uint32_t *insn) {
    const hos_cpu_module *m = module_at(pc);
    if (!m) {
        return 0;
    }
    uint64_t text = m->image_base + m->text_off;
    if (pc < text || pc + 4 > text + m->text_size) {
        return 0;
    }
    memcpy(insn, (const void *)pc, 4);
    return 1;
}

static void log_module_offset(const char *what, uint64_t addr) {
    const hos_cpu_module *m = module_at(addr);
    if (m) {
        HOS_LOGE(MOD, "  %s  %#018" PRIx64 "  = %s+%#" PRIx64, what, addr,
                 m->name ? m->name : "image", addr - m->image_base);
    } else {
        HOS_LOGE(MOD, "  %s  %#018" PRIx64, what, addr);
    }
}

static void dump_and_die(int sig, siginfo_t *si, ucontext_t *uc, int in_game) {
    HOS_LOGE(MOD, "fatal %s (code %d) in %s context", sig_name(sig), si->si_code,
             in_game ? "game" : "host");
    log_module_offset("pc", (uint64_t)uc->uc_mcontext.pc);
    log_module_offset("lr", (uint64_t)uc->uc_mcontext.regs[30]);
    HOS_LOGE(MOD, "  sp  %#018" PRIx64 "  fault addr %p", (uint64_t)uc->uc_mcontext.sp,
             si->si_addr);
    for (int i = 0; i < 30; i += 2) {
        HOS_LOGE(MOD, "  x%-2d %#018" PRIx64 "  x%-2d %#018" PRIx64, i,
                 (uint64_t)uc->uc_mcontext.regs[i], i + 1, (uint64_t)uc->uc_mcontext.regs[i + 1]);
    }
    if (in_game && hos_current_thread) {
        HOS_LOGE(MOD, "  thread handle %#x tid %u", hos_current_thread->handle,
                 hos_current_thread->tid);
    }
    hos_mem_dump();
    _exit(128 + sig);
}

void hos_trap_c(int sig, siginfo_t *si, void *ucv, int in_game) {
    ucontext_t *uc = ucv;
    uint64_t pc = (uint64_t)uc->uc_mcontext.pc;
    uint32_t insn;

    if (sig == SIGTRAP && in_game && read_text_insn(pc, &insn) &&
        (insn & A64_SVC_MASK) == A64_BRK_BITS) {
        uint32_t imm = (insn >> 5) & 0xFFFF;
        /* Counter reads rewritten by the patcher: the register number rides in the immediate. */
        if (imm >= A64_BRK_TICK_BASE && imm < A64_BRK_TICK_BASE + 31) {
            if (g_stats) {
                atomic_fetch_add_explicit(&g_tick_count, 1, memory_order_relaxed);
            }
            uc->uc_mcontext.regs[imm - A64_BRK_TICK_BASE] = hos_tick_now();
            uc->uc_mcontext.pc = pc + 4;
            return;
        }
        if (imm >= A64_BRK_CNTFRQ_BASE && imm < A64_BRK_CNTFRQ_BASE + 31) {
            uc->uc_mcontext.regs[imm - A64_BRK_CNTFRQ_BASE] = HOS_TICK_FREQ;
            uc->uc_mcontext.pc = pc + 4;
            return;
        }
        if (imm >= 1 && imm <= HOS_SVC_MAX) {
            if (g_stats) {
                atomic_fetch_add_explicit(&g_svc_count[imm], 1, memory_order_relaxed);
            }
            hos_svc_regs r;
            for (int i = 0; i < 8; i++) {
                r.x[i] = uc->uc_mcontext.regs[i];
            }
            hos_svc_dispatch(imm, &r);
            for (int i = 0; i < 8; i++) {
                uc->uc_mcontext.regs[i] = r.x[i];
            }
            uc->uc_mcontext.pc = pc + 4;
            return;
        }
    }
    dump_and_die(sig, si, uc, in_game);
}

void hos_trap_install(void) {
    const char *st = getenv("HOS_TRAP_STATS");
    if (st && *st && strcmp(st, "0") != 0 && !g_stats) {
        g_stats = 1;
        g_stats_start_ns = mono_ns();
        atexit(print_stats);
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = hos_trap_entry;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    static const int sigs[] = {SIGTRAP, SIGSEGV, SIGBUS, SIGILL, SIGFPE};
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
        if (sigaction(sigs[i], &sa, NULL) != 0) {
            HOS_LOGE(MOD, "sigaction(%s) failed", sig_name(sigs[i]));
        }
    }
}

#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

#include "common/hos_log.h"
#include "cpu_internal.h"

#define MOD "svc"

/* GetInfo ids (switchbrew InfoType). */
enum {
    INFO_CORE_MASK = 0,
    INFO_PRIORITY_MASK = 1,
    INFO_ALIAS_REGION_ADDRESS = 2,
    INFO_ALIAS_REGION_SIZE = 3,
    INFO_HEAP_REGION_ADDRESS = 4,
    INFO_HEAP_REGION_SIZE = 5,
    INFO_TOTAL_MEMORY_SIZE = 6,
    INFO_USED_MEMORY_SIZE = 7,
    INFO_DEBUGGER_ATTACHED = 8,
    INFO_RESOURCE_LIMIT = 9,
    INFO_IDLE_TICK_COUNT = 10,
    INFO_RANDOM_ENTROPY = 11,
    INFO_ASLR_REGION_ADDRESS = 12,
    INFO_ASLR_REGION_SIZE = 13,
    INFO_STACK_REGION_ADDRESS = 14,
    INFO_STACK_REGION_SIZE = 15,
    INFO_SYSTEM_RESOURCE_SIZE_TOTAL = 16,
    INFO_SYSTEM_RESOURCE_SIZE_USED = 17,
    INFO_PROGRAM_ID = 18,
    INFO_INITIAL_PROCESS_ID_RANGE = 19,
    INFO_USER_EXCEPTION_CONTEXT_ADDRESS = 20,
    INFO_TOTAL_NON_SYSTEM_MEMORY_SIZE = 21,
    INFO_USED_NON_SYSTEM_MEMORY_SIZE = 22,
    INFO_IS_APPLICATION = 23,
    INFO_FREE_THREAD_COUNT = 24,
    INFO_THREAD_TICK_COUNT = 25,
    INFO_IS_SVC_PERMITTED = 26,
    INFO_IO_REGION_HINT = 27,
    INFO_ALIAS_REGION_EXTRA_SIZE = 28,
    INFO_THREAD_TICK_COUNT_DEPRECATED = 0xF0000002,
};

/* What a retail application sees on a 4 GiB Switch. */
#define APPLICATION_MEMORY_SIZE 0xCD500000ull
#define SYSTEM_RESOURCE_SIZE    0x1000000ull

#define BREAK_NOTIFICATION_ONLY 0x80000000u

static uint64_t read_cntvct(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

/*
 * The 19.2 MHz counter the game sees. Games and libnx hardcode that frequency
 * when converting ticks, so the host counter cannot be exposed directly; this
 * is derived from CLOCK_MONOTONIC, the same base the time service uses for its
 * steady clock.
 */
uint64_t hos_tick_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    unsigned __int128 ns = (unsigned __int128)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
    return (uint64_t)(ns * HOS_TICK_FREQ / 1000000000u);
}

void svc_get_system_tick(hos_svc_regs *r) {
    r->x[0] = hos_tick_now();
}

void svc_exit_process(hos_svc_regs *r) {
    (void)r;
    HOS_LOGI(MOD, "svcExitProcess");
    exit(0);
}

void svc_create_thread(hos_svc_regs *r) {
    uint32_t handle = 0;
    hos_result res =
        hos_thread_create(r->x[1], r->x[2], r->x[3], (int32_t)r->x[4], (int32_t)r->x[5], &handle);
    r->x[0] = res;
    r->x[1] = handle;
}

void svc_start_thread(hos_svc_regs *r) {
    r->x[0] = hos_thread_start((uint32_t)r->x[0]);
}

void svc_exit_thread(hos_svc_regs *r) {
    (void)r;
    hos_thread_exit_current();
}

void svc_get_thread_priority(hos_svc_regs *r) {
    hos_thread *t = hos_thread_from_handle((uint32_t)r->x[1]);
    if (!t) {
        r->x[0] = HOS_KERNEL_INVALID_HANDLE;
        return;
    }
    r->x[0] = HOS_RESULT_SUCCESS;
    r->x[1] = (uint32_t)t->priority;
}

void svc_set_thread_priority(hos_svc_regs *r) {
    hos_thread *t = hos_thread_from_handle((uint32_t)r->x[0]);
    if (!t) {
        r->x[0] = HOS_KERNEL_INVALID_HANDLE;
        return;
    }
    /* sync.c reads it when choosing which waiter to wake. */
    __atomic_store_n(&t->priority, (int32_t)r->x[1], __ATOMIC_RELAXED);
    r->x[0] = HOS_RESULT_SUCCESS;
}

void svc_get_thread_core_mask(hos_svc_regs *r) {
    hos_thread *t = hos_thread_from_handle((uint32_t)r->x[2]);
    if (!t) {
        r->x[0] = HOS_KERNEL_INVALID_HANDLE;
        return;
    }
    r->x[0] = HOS_RESULT_SUCCESS;
    r->x[1] = (uint64_t)(int64_t)t->core;
    r->x[2] = 0xF;
}

void svc_set_thread_core_mask(hos_svc_regs *r) {
    hos_thread *t = hos_thread_from_handle((uint32_t)r->x[0]);
    if (!t) {
        r->x[0] = HOS_KERNEL_INVALID_HANDLE;
        return;
    }
    t->core = (int32_t)r->x[1];
    r->x[0] = HOS_RESULT_SUCCESS;
}

void svc_get_current_processor_number(hos_svc_regs *r) {
    int cpu = sched_getcpu();
    r->x[0] = (uint64_t)(cpu < 0 ? 0 : cpu & 3);
}

/* svcCloseHandle(w0): drop the handle and release whatever it referenced. */
void svc_close_handle(hos_svc_regs *r) {
    uint32_t h = (uint32_t)r->x[0];
    hos_obj_type type;
    void *obj = hos_handle_lookup(h, &type);
    hos_result res = hos_handle_close(h);
    if (HOS_FAILED(res) || !obj) {
        r->x[0] = res;
        return;
    }
    switch (type) {
    case HOS_OBJ_THREAD:
        hos_thread_handle_closed(obj);
        break;
    case HOS_OBJ_READABLE_EVENT:
    case HOS_OBJ_WRITABLE_EVENT:
        hos_event_release(obj);
        break;
    case HOS_OBJ_SESSION:
        hos_session_close(obj);
        break;
    case HOS_OBJ_SHARED_MEMORY:
        hos_shmem_release(obj);
        break;
    case HOS_OBJ_TRANSFER_MEMORY:
        hos_tmem_release(obj);
        break;
    default:
        break;
    }
    r->x[0] = HOS_RESULT_SUCCESS;
}

void svc_get_process_id(hos_svc_regs *r) {
    uint32_t h = (uint32_t)r->x[1];
    if (h != HOS_HANDLE_CURRENT_PROCESS && !hos_handle_get(h, HOS_OBJ_PROCESS)) {
        r->x[0] = HOS_KERNEL_INVALID_HANDLE;
        return;
    }
    r->x[0] = HOS_RESULT_SUCCESS;
    r->x[1] = (uint64_t)getpid();
}

void svc_get_thread_id(hos_svc_regs *r) {
    hos_thread *t = hos_thread_from_handle((uint32_t)r->x[1]);
    if (!t) {
        r->x[0] = HOS_KERNEL_INVALID_HANDLE;
        return;
    }
    r->x[0] = HOS_RESULT_SUCCESS;
    r->x[1] = t->tid;
}

static const char *break_reason_name(uint32_t reason) {
    static const char *const names[] = {"Panic",         "Assert",      "User",
                                        "PreLoadDll",    "PostLoadDll", "PreUnloadDll",
                                        "PostUnloadDll", "CppException"};
    uint32_t r = reason & ~BREAK_NOTIFICATION_ONLY;
    return r < sizeof(names) / sizeof(names[0]) ? names[r] : "?";
}

void svc_break(hos_svc_regs *r) {
    uint32_t reason = (uint32_t)r->x[0];
    uint64_t addr = r->x[1];
    uint64_t size = r->x[2];
    int notify = (reason & BREAK_NOTIFICATION_ONLY) != 0;

    if (size == 4 && addr) {
        uint32_t v;
        memcpy(&v, (const void *)addr, 4);
        hos_log(notify ? HOS_LOG_INFO : HOS_LOG_ERROR, MOD,
                "svcBreak %s%s result %#x (module %u desc %u)", break_reason_name(reason),
                notify ? " [notification]" : "", v, HOS_RESULT_MODULE(v), HOS_RESULT_DESC(v));
    } else {
        hos_log(notify ? HOS_LOG_INFO : HOS_LOG_ERROR, MOD,
                "svcBreak %s%s addr %#" PRIx64 " size %#" PRIx64, break_reason_name(reason),
                notify ? " [notification]" : "", addr, size);
    }
    if (notify) {
        return;
    }
    const char *mode = getenv("HOS_BREAK");
    if (mode && strcmp(mode, "continue") == 0) {
        HOS_LOGW(MOD, "HOS_BREAK=continue: resuming after svcBreak");
        return;
    }
    /* Stop like the real kernel would. Under GDB this is a clean SIGABRT stop. */
    abort();
}

void svc_output_debug_string(hos_svc_regs *r) {
    const char *p = (const char *)r->x[0];
    size_t n = r->x[1];
    char buf[1024];
    if (!p) {
        r->x[0] = HOS_KERNEL_INVALID_POINTER;
        return;
    }
    if (n >= sizeof(buf)) {
        n = sizeof(buf) - 1;
    }
    memcpy(buf, p, n);
    hos_log_guest(HOS_LOG_INFO, NULL, buf, n);
    r->x[0] = HOS_RESULT_SUCCESS;
}

void svc_flush_cache_noop(hos_svc_regs *r) {
    /* Data and instruction caches are coherent enough for our purposes; the
     * patcher never writes code after mprotect, and the game cannot either. */
    r->x[0] = HOS_RESULT_SUCCESS;
}

void svc_get_info(hos_svc_regs *r) {
    uint32_t id = (uint32_t)r->x[1];
    uint32_t handle = (uint32_t)r->x[2];
    uint64_t sub = r->x[3];
    uint64_t v = 0;
    (void)handle;

    switch (id) {
    case INFO_CORE_MASK:
        v = 0xF;
        break;
    case INFO_PRIORITY_MASK:
        v = 0xFFFFFFFFFFFFFFF0ull; /* priorities 4..63 */
        break;
    case INFO_ALIAS_REGION_ADDRESS:
        v = HOS_ALIAS_BASE;
        break;
    case INFO_ALIAS_REGION_SIZE:
        v = HOS_ALIAS_SIZE;
        break;
    case INFO_HEAP_REGION_ADDRESS:
        v = HOS_HEAP_BASE;
        break;
    case INFO_HEAP_REGION_SIZE:
        v = HOS_HEAP_MAX;
        break;
    case INFO_TOTAL_MEMORY_SIZE:
    case INFO_TOTAL_NON_SYSTEM_MEMORY_SIZE:
        v = APPLICATION_MEMORY_SIZE;
        break;
    case INFO_USED_MEMORY_SIZE:
    case INFO_USED_NON_SYSTEM_MEMORY_SIZE:
        v = hos_heap_size + hos_main_stack_size;
        for (uint32_t i = 0; i < hos_params.num_modules; i++) {
            v += hos_params.modules[i].image_size;
        }
        break;
    case INFO_DEBUGGER_ATTACHED:
        v = 0;
        break;
    case INFO_RANDOM_ENTROPY:
        if (sub > 3) {
            r->x[0] = HOS_KERNEL_INVALID_COMBINATION;
            return;
        }
        if (getrandom(&v, sizeof(v), 0) != (ssize_t)sizeof(v)) {
            v = read_cntvct() * 6364136223846793005ull + sub;
        }
        break;
    case INFO_ASLR_REGION_ADDRESS:
        v = HOS_ASLR_BASE;
        break;
    case INFO_ASLR_REGION_SIZE:
        v = HOS_ASLR_SIZE;
        break;
    case INFO_STACK_REGION_ADDRESS:
        v = HOS_STACK_BASE;
        break;
    case INFO_STACK_REGION_SIZE:
        v = HOS_STACK_SIZE;
        break;
    case INFO_SYSTEM_RESOURCE_SIZE_TOTAL:
        v = SYSTEM_RESOURCE_SIZE;
        break;
    case INFO_SYSTEM_RESOURCE_SIZE_USED:
        v = 0;
        break;
    case INFO_PROGRAM_ID:
        v = hos_params.program_id;
        break;
    case INFO_INITIAL_PROCESS_ID_RANGE:
        v = (sub == 0) ? 1 : 0x50;
        break;
    case INFO_USER_EXCEPTION_CONTEXT_ADDRESS:
        v = 0;
        break;
    case INFO_IS_APPLICATION:
        v = 1;
        break;
    case INFO_FREE_THREAD_COUNT:
        v = 0x100;
        break;
    case INFO_THREAD_TICK_COUNT:
    case INFO_THREAD_TICK_COUNT_DEPRECATED:
        v = read_cntvct();
        break;
    case INFO_IS_SVC_PERMITTED:
        v = 1;
        break;
    case INFO_IO_REGION_HINT:
        v = 0;
        break;
    case INFO_ALIAS_REGION_EXTRA_SIZE:
        v = 0;
        break;
    default:
        HOS_LOGW(MOD, "svcGetInfo: unknown id %u (sub %#" PRIx64 ")", id, sub);
        r->x[0] = HOS_KERNEL_INVALID_ENUM_VALUE;
        return;
    }
    r->x[0] = HOS_RESULT_SUCCESS;
    r->x[1] = v;
}

/* ---- exit request (see hos_ipc.h) ---------------------------------------------- */

static void (*g_exit_handler)(void);

void hos_applet_set_exit_handler(void (*fn)(void)) {
    g_exit_handler = fn;
}

void hos_applet_request_exit(void) {
    if (g_exit_handler) {
        g_exit_handler();
    } else {
        exit(0);
    }
}

/* ---- external pad (see hos_ipc.h) --------------------------------------------- */

static pthread_mutex_t g_pad_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_pad_active;
static uint64_t g_pad_buttons;
static int32_t g_pad_sticks[4];

void hos_pad_external_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    pthread_mutex_lock(&g_pad_lock);
    g_pad_active = 1;
    g_pad_buttons = buttons;
    g_pad_sticks[0] = lx;
    g_pad_sticks[1] = ly;
    g_pad_sticks[2] = rx;
    g_pad_sticks[3] = ry;
    pthread_mutex_unlock(&g_pad_lock);
}

int hos_pad_external_get(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    pthread_mutex_lock(&g_pad_lock);
    int active = g_pad_active;
    if (active) {
        *buttons = g_pad_buttons;
        *lx = g_pad_sticks[0];
        *ly = g_pad_sticks[1];
        *rx = g_pad_sticks[2];
        *ry = g_pad_sticks[3];
    }
    pthread_mutex_unlock(&g_pad_lock);
    return active;
}

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "common/hos_log.h"
#include "cpu_internal.h"

#define MOD "thread"

uint64_t hos_tls_arena_base;
uint64_t hos_tls_arena_size;
__thread hos_thread *hos_current_thread;

/* One bit per TLS page. */
#define TLS_SLOTS (HOS_TLS_ARENA_SIZE / HOS_TLS_SIZE)
static uint64_t g_tls_bitmap[TLS_SLOTS / 64];
static pthread_mutex_t g_tls_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_thread_lock = PTHREAD_MUTEX_INITIALIZER;

hos_result hos_tls_arena_init(void) {
    void *p = mmap((void *)HOS_TLS_ARENA_BASE, HOS_TLS_ARENA_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        HOS_LOGE(MOD, "cannot reserve TLS arena: %s", strerror(errno));
        return HOS_KERNEL_OUT_OF_MEMORY;
    }
    hos_tls_arena_base = (uint64_t)p;
    hos_tls_arena_size = HOS_TLS_ARENA_SIZE;
    hos_mem_set(hos_tls_arena_base, hos_tls_arena_size, HOS_MEM_THREAD_LOCAL, HOS_PERM_RW, 0);
    HOS_LOGD(MOD, "TLS arena at %#" PRIx64 " (%#" PRIx64 " bytes)", hos_tls_arena_base,
             hos_tls_arena_size);
    return HOS_RESULT_SUCCESS;
}

uint64_t hos_tls_alloc(void) {
    pthread_mutex_lock(&g_tls_lock);
    for (size_t w = 0; w < TLS_SLOTS / 64; w++) {
        if (g_tls_bitmap[w] == UINT64_MAX) {
            continue;
        }
        int bit = __builtin_ctzll(~g_tls_bitmap[w]);
        g_tls_bitmap[w] |= 1ull << bit;
        pthread_mutex_unlock(&g_tls_lock);
        uint64_t tls = hos_tls_arena_base + (w * 64 + (size_t)bit) * HOS_TLS_SIZE;
        memset((void *)tls, 0, HOS_TLS_SIZE);
        return tls;
    }
    pthread_mutex_unlock(&g_tls_lock);
    return 0;
}

void hos_tls_free(uint64_t tls) {
    size_t slot = (tls - hos_tls_arena_base) / HOS_TLS_SIZE;
    pthread_mutex_lock(&g_tls_lock);
    g_tls_bitmap[slot / 64] &= ~(1ull << (slot % 64));
    pthread_mutex_unlock(&g_tls_lock);
}

static uint64_t read_host_tp(void) {
    uint64_t tp;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    return tp;
}

static hos_thread *thread_alloc(void) {
    void *p =
        mmap(NULL, sizeof(hos_thread), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        return NULL;
    }
    memset(p, 0, sizeof(hos_thread));
    return p;
}

static void thread_free(hos_thread *t) {
    if (t->tls) {
        hos_tls_free(t->tls);
    }
    sem_destroy(&t->start_sem);
    munmap(t, sizeof(*t));
}

/* Make the calling host thread the owner of `t` and prepare it to run game code. */
static hos_result bind_current(hos_thread *t) {
    t->tid = (uint32_t)syscall(SYS_gettid);
    hos_current_thread = t;

    *(uint64_t *)(t->tls + HOS_TLS_HOST_TP) = read_host_tp();
    *(uint64_t *)(t->tls + HOS_TLS_THREAD) = (uint64_t)t;

    t->altstack =
        mmap(NULL, HOS_ALT_STACK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (t->altstack == MAP_FAILED) {
        t->altstack = NULL;
        return HOS_KERNEL_OUT_OF_MEMORY;
    }
    stack_t ss = {.ss_sp = t->altstack, .ss_size = HOS_ALT_STACK_SIZE, .ss_flags = 0};
    if (sigaltstack(&ss, NULL) != 0) {
        HOS_LOGE(MOD, "sigaltstack: %s", strerror(errno));
        return HOS_KERNEL_INVALID_STATE;
    }
    /* The creator may have been inside a trap; make sure nothing stays blocked. */
    sigset_t none;
    sigemptyset(&none);
    pthread_sigmask(SIG_SETMASK, &none, NULL);
    return HOS_RESULT_SUCCESS;
}

static void unbind_current(hos_thread *t) {
    stack_t ss = {.ss_flags = SS_DISABLE};
    sigaltstack(&ss, NULL);
    if (t->altstack) {
        munmap(t->altstack, HOS_ALT_STACK_SIZE);
        t->altstack = NULL;
    }
    hos_current_thread = NULL;
}

/*
 * The thread is gone. Wake anyone waiting on it, then decide under the thread
 * lock which side frees the struct: whoever comes second (this thread or the
 * CloseHandle from the game) sees the other flag already set and frees.
 */
static void thread_finished(hos_thread *t) {
    hos_sync_thread_exited(t);
    pthread_mutex_lock(&g_thread_lock);
    t->finished = 1;
    int free_now = t->handle_closed;
    pthread_mutex_unlock(&g_thread_lock);
    if (free_now) {
        thread_free(t);
    }
}

void hos_thread_handle_closed(hos_thread *t) {
    if (t->is_main) {
        return;
    }
    pthread_mutex_lock(&g_thread_lock);
    t->handle_closed = 1;
    int free_now = t->finished;
    pthread_mutex_unlock(&g_thread_lock);
    if (free_now) {
        thread_free(t);
    }
}

static void *thread_main(void *arg) {
    hos_thread *t = arg;
    while (sem_wait(&t->start_sem) != 0 && errno == EINTR) {
    }
    if (HOS_FAILED(bind_current(t))) {
        HOS_LOGE(MOD, "thread %#x: cannot bind host thread", t->handle);
        thread_finished(t);
        return NULL;
    }
    HOS_LOGD(MOD, "thread %#x (tid %u) entering %#" PRIx64 " sp %#" PRIx64, t->handle, t->tid,
             t->entry, t->stack_top);
    if (sigsetjmp(t->exit_jb, 1) == 0) {
        hos_enter(t->entry, t->arg, 0, t->stack_top, t->tls, 0);
    }
    HOS_LOGD(MOD, "thread %#x (tid %u) exited", t->handle, t->tid);
    unbind_current(t);
    thread_finished(t);
    return NULL;
}

hos_result hos_thread_create(uint64_t entry, uint64_t arg, uint64_t stack_top, int32_t priority,
                             int32_t core, uint32_t *out_handle) {
    hos_thread *t = thread_alloc();
    if (!t) {
        return HOS_KERNEL_OUT_OF_MEMORY;
    }
    t->entry = entry;
    t->arg = arg;
    t->stack_top = stack_top;
    t->priority = priority;
    t->core = core;
    t->tls = hos_tls_alloc();
    if (!t->tls) {
        munmap(t, sizeof(*t));
        return HOS_KERNEL_OUT_OF_RESOURCE;
    }
    sem_init(&t->start_sem, 0, 0);
    t->handle = hos_handle_alloc(HOS_OBJ_THREAD, t);
    if (!t->handle) {
        thread_free(t);
        return HOS_KERNEL_OUT_OF_HANDLES;
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    /* The host stack only carries shim frames; game code runs on stack_top. */
    pthread_attr_setstacksize(&attr, 256 * 1024);
    int rc = pthread_create(&t->pt, &attr, thread_main, t);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        HOS_LOGE(MOD, "pthread_create: %s", strerror(rc));
        hos_handle_close(t->handle);
        thread_free(t);
        return HOS_KERNEL_OUT_OF_RESOURCE;
    }
    *out_handle = t->handle;
    HOS_LOGD(MOD, "created thread %#x entry %#" PRIx64 " prio %d core %d", t->handle, entry,
             priority, core);
    return HOS_RESULT_SUCCESS;
}

hos_thread *hos_thread_from_handle(uint32_t handle) {
    if (handle == HOS_HANDLE_CURRENT_THREAD) {
        return hos_current_thread;
    }
    return hos_handle_get(handle, HOS_OBJ_THREAD);
}

hos_result hos_thread_start(uint32_t handle) {
    hos_thread *t = hos_thread_from_handle(handle);
    if (!t) {
        return HOS_KERNEL_INVALID_HANDLE;
    }
    sem_post(&t->start_sem);
    return HOS_RESULT_SUCCESS;
}

void hos_thread_exit_current(void) {
    hos_thread *t = hos_current_thread;
    if (!t) {
        HOS_LOGE(MOD, "svcExitThread from a non-game thread");
        _exit(1);
    }
    siglongjmp(t->exit_jb, 1);
}

uint64_t hos_main_stack_base;
uint64_t hos_main_stack_size;

static hos_thread *make_bound_thread(int is_main, uint64_t entry, uint64_t stack_top) {
    hos_thread *t = thread_alloc();
    if (!t) {
        return NULL;
    }
    t->is_main = is_main;
    t->entry = entry;
    t->stack_top = stack_top;
    t->priority = 44;
    t->tls = hos_tls_alloc();
    t->handle = hos_handle_alloc(HOS_OBJ_THREAD, t);
    t->pt = pthread_self();
    sem_init(&t->start_sem, 0, 0);
    if (!t->tls || !t->handle || HOS_FAILED(bind_current(t))) {
        return NULL;
    }
    return t;
}

uint32_t hos_cpu_adopt_current_thread(void) {
    if (hos_current_thread) {
        return hos_current_thread->handle;
    }
    hos_thread *t = make_bound_thread(1, 0, 0);
    return t ? t->handle : 0;
}

void hos_cpu_run_main(uint64_t entry, uint64_t stack_size) {
    hos_cpu_run_main_with(entry, stack_size, NULL, NULL);
}

void hos_cpu_run_main_with(uint64_t entry, uint64_t stack_size,
                           void (*prepare)(uint32_t main_thread, hos_entry_args *args, void *user),
                           void *user) {
    stack_size = (stack_size + 0xFFF) & ~0xFFFull;
    /*
     * The stack must live inside the 39-bit HOS address space: the game passes
     * stack buffers through IPC descriptors that cannot encode anything higher.
     * Take it from the top of the reserved stack region, guard page below.
     */
    if (stack_size + 0x1000 > HOS_STACK_SIZE / 4) {
        HOS_LOGE(MOD, "main stack of %#" PRIx64 " bytes is too large", stack_size);
        exit(1);
    }
    uint8_t *stack = (uint8_t *)(HOS_STACK_BASE + HOS_STACK_SIZE - stack_size - 0x1000);
    if (mmap(stack, stack_size + 0x1000, PROT_NONE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0) == MAP_FAILED ||
        mprotect(stack + 0x1000, stack_size, PROT_READ | PROT_WRITE) != 0) {
        HOS_LOGE(MOD, "cannot allocate main stack: %s", strerror(errno));
        exit(1);
    }
    hos_main_stack_base = (uint64_t)stack + 0x1000;
    hos_main_stack_size = stack_size;
    hos_mem_set(hos_main_stack_base, stack_size, HOS_MEM_STACK, HOS_PERM_RW, 0);

    hos_thread *t = make_bound_thread(1, entry, hos_main_stack_base + stack_size);
    if (!t) {
        HOS_LOGE(MOD, "cannot set up the main thread");
        exit(1);
    }

    HOS_LOGI(MOD,
             "main thread %#x: entry %#" PRIx64 " stack [%#" PRIx64 ", %#" PRIx64 ") tls %#" PRIx64,
             t->handle, entry, hos_main_stack_base, t->stack_top, t->tls);
    hos_entry_args args = {0, t->handle, 0};
    if (prepare) {
        prepare(t->handle, &args, user);
    }
    if (sigsetjmp(t->exit_jb, 1) == 0) {
        hos_enter(entry, args.x0, args.x1, t->stack_top, t->tls, args.lr);
    }
    /* svcExitThread on the main thread: other game threads may still run. */
    HOS_LOGD(MOD, "main thread exited; waiting for remaining threads");
    unbind_current(t);
    hos_sync_thread_exited(t);
    pthread_exit(NULL);
}

void *hos_cpu_current_tls(void) {
    return hos_current_thread ? (void *)hos_current_thread->tls : NULL;
}

/*
 * libhos_cpu integration: call the dispatcher directly and spin real game
 * threads through the brk trap path. ARM64 only.
 */

#include <pthread.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>

#include "common/hos_log.h"
#include "hos_cpu.h"
#include "test.h"

#define HEAP_BASE    0x1000000000ull
#define STACK_REGION 0x3000000000ull
#define MUTEX_FLAG   0x40000000u

/* Game thread bodies. They run with the HOS TLS in tpidr_el0, so no libc. */
__asm__(".text\n"
        ".globl test_thread_entry\n"
        ".type test_thread_entry, %function\n"
        "test_thread_entry:\n"
        "    mov x1, #1\n"
        "    str x1, [x0]\n"
        "    brk #0x0A\n"
        "    b .\n"
        ".size test_thread_entry, . - test_thread_entry\n"
        /* x0 = {u32 *mutex, u32 self, u32 *counter}: lock, ++, unlock, exit */
        ".globl test_mutex_entry\n"
        ".type test_mutex_entry, %function\n"
        "test_mutex_entry:\n"
        "    mov x19, x0\n"
        "    ldr x20, [x19]\n"      /* mutex */
        "    ldr w21, [x19, #8]\n"  /* self */
        "    ldr x22, [x19, #16]\n" /* counter */
        "1:  ldaxr w0, [x20]\n"
        "    cbnz w0, 2f\n"
        "    stlxr w1, w21, [x20]\n"
        "    cbnz w1, 1b\n"
        "    b 4f\n"
        "2:  orr w1, w0, #0x40000000\n"
        "    ldaxr w3, [x20]\n"
        "    cmp w3, w0\n"
        "    b.ne 1b\n"
        "    stlxr w3, w1, [x20]\n"
        "    cbnz w3, 1b\n"
        "    and w0, w0, #0x3fffffff\n" /* ArbitrateLock(owner, mutex, self) */
        "    mov x1, x20\n"
        "    mov w2, w21\n"
        "    brk #0x1A\n"
        "    ldr w0, [x20]\n"
        "    and w0, w0, #0x3fffffff\n"
        "    cmp w0, w21\n"
        "    b.ne 1b\n"
        "4:  ldr w0, [x22]\n"
        "    add w0, w0, #1\n"
        "    str w0, [x22]\n"
        "    mov x0, #0\n" /* SleepThread(0): yield while holding the lock */
        "    brk #0x0B\n"
        "7:  ldaxr w1, [x20]\n" /* unlock: CAS self -> 0, else ArbitrateUnlock */
        "    cmp w1, w21\n"
        "    b.ne 5f\n"
        "    stlxr w3, wzr, [x20]\n"
        "    cbnz w3, 7b\n"
        "    b 6f\n"
        "5:  clrex\n"
        "    mov x0, x20\n"
        "    brk #0x1B\n"
        "6:  brk #0x0A\n"
        "    b .\n"
        ".size test_mutex_entry, . - test_mutex_entry\n");
extern void test_thread_entry(void);
extern void test_mutex_entry(void);

/*
 * Wake-order bodies. x0 = wake_args (see below): raise the flag, park on the
 * key (address arbiter, a mutex owned by the test, or a condvar with the
 * thread's own mutex), and on waking append the id to order[] through an
 * atomic counter, then exit. The mutex body hands the mutex on with
 * ArbitrateUnlock, so one unlock from the test drains the whole queue.
 */
#define WAKE_PROLOGUE                                                                              \
    "    mov x19, x0\n"                                                                            \
    "    ldr x20, [x19]\n"      /* key */                                                          \
    "    ldr x21, [x19, #8]\n"  /* order[] */                                                      \
    "    ldr x22, [x19, #16]\n" /* count */                                                        \
    "    ldr w23, [x19, #24]\n" /* id */                                                           \
    "    ldr x24, [x19, #32]\n" /* flag */                                                         \
    "    ldr w25, [x19, #40]\n" /* self */                                                         \
    "    ldr x26, [x19, #48]\n" /* own mutex */                                                    \
    "    mov w0, #1\n"                                                                             \
    "    stlr w0, [x24]\n"
#define WAKE_RECORD                                                                                \
    "8:  ldaxr w0, [x22]\n"                                                                        \
    "    add w1, w0, #1\n"                                                                         \
    "    stlxr w2, w1, [x22]\n"                                                                    \
    "    cbnz w2, 8b\n"                                                                            \
    "    str w23, [x21, x0, lsl #2]\n"
__asm__(".text\n"
        ".globl test_wake_arbiter_entry\n"
        ".type test_wake_arbiter_entry, %function\n"
        "test_wake_arbiter_entry:\n" WAKE_PROLOGUE
        "    mov x0, x20\n" /* WaitForAddress(key, WaitIfEqual, 0, forever) */
        "    mov w1, #2\n"
        "    mov w2, #0\n"
        "    mov x3, #-1\n"
        "    brk #0x34\n" WAKE_RECORD "    brk #0x0A\n"
        "    b .\n"
        ".size test_wake_arbiter_entry, . - test_wake_arbiter_entry\n"
        ".globl test_wake_mutex_entry\n"
        ".type test_wake_mutex_entry, %function\n"
        "test_wake_mutex_entry:\n" WAKE_PROLOGUE
        "2:  ldaxr w0, [x20]\n" /* mark the owner's word: someone is parked */
        "    orr w1, w0, #0x40000000\n"
        "    stlxr w2, w1, [x20]\n"
        "    cbnz w2, 2b\n"
        "    and w0, w0, #0x3fffffff\n" /* ArbitrateLock(owner, key, self) */
        "    mov x1, x20\n"
        "    mov w2, w25\n"
        "    brk #0x1A\n" WAKE_RECORD
        "    mov x0, x20\n" /* ArbitrateUnlock(key): hand it to the next waiter */
        "    brk #0x1B\n"
        "    brk #0x0A\n"
        "    b .\n"
        ".size test_wake_mutex_entry, . - test_wake_mutex_entry\n"
        ".globl test_wake_condvar_entry\n"
        ".type test_wake_condvar_entry, %function\n"
        "test_wake_condvar_entry:\n" WAKE_PROLOGUE
        "    mov x0, x26\n" /* WaitProcessWideKeyAtomic(own mutex, key, self, forever) */
        "    mov x1, x20\n"
        "    mov w2, w25\n"
        "    mov x3, #-1\n"
        "    brk #0x1C\n" WAKE_RECORD "    brk #0x0A\n"
        "    b .\n"
        ".size test_wake_condvar_entry, . - test_wake_condvar_entry\n"
        ".globl test_wake_condvar_mutex_entry\n"
        ".type test_wake_condvar_mutex_entry, %function\n"
        "test_wake_condvar_mutex_entry:\n" WAKE_PROLOGUE
        "    mov x0, x26\n" /* as above, then hand the (shared) mutex on */
        "    mov x1, x20\n"
        "    mov w2, w25\n"
        "    mov x3, #-1\n"
        "    brk #0x1C\n" WAKE_RECORD "    mov x0, x26\n"
        "    brk #0x1B\n"
        "    brk #0x0A\n"
        "    b .\n"
        ".size test_wake_condvar_mutex_entry, . - test_wake_condvar_mutex_entry\n");
extern void test_wake_arbiter_entry(void);
extern void test_wake_mutex_entry(void);
extern void test_wake_condvar_entry(void);
extern void test_wake_condvar_mutex_entry(void);

static int g_inited;

static void init_once(void) {
    if (g_inited) {
        return;
    }
    g_inited = 1;
    hos_log_set_level(HOS_LOG_WARN);
    /* Pretend the test binary pages holding the asm bodies are the game text. */
    uint64_t text = (uint64_t)test_thread_entry & ~0xFFFull;
    hos_cpu_params p = {
        .num_modules = 1,
        .modules =
            {{.image_base = text, .image_size = 0x4000, .text_size = 0x4000, .name = "test_svc"}},
        .program_id = 0x0100000000010000ull,
    };
    if (HOS_FAILED(hos_cpu_init(&p)) || !hos_cpu_adopt_current_thread()) {
        fprintf(stderr, "init failed\n");
        exit(1);
    }
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static hos_svc_regs call(uint32_t id, uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3) {
    hos_svc_regs r = {{x0, x1, x2, x3, 0, 0, 0, 0}};
    hos_svc_dispatch(id, &r);
    return r;
}

static uint32_t create_thread_prio(void (*entry)(void), void *arg, int32_t prio, void **stack_out) {
    size_t stack_size = 0x10000;
    uint8_t *stack =
        mmap(NULL, stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack == MAP_FAILED) {
        return 0;
    }
    *stack_out = stack;
    hos_svc_regs r = {{0, (uint64_t)entry, (uint64_t)arg, (uint64_t)stack + stack_size,
                       (uint64_t)(uint32_t)prio, 0, 0, 0}};
    hos_svc_dispatch(0x08, &r);
    return r.x[0] == 0 ? (uint32_t)r.x[1] : 0;
}

static uint32_t create_thread(void (*entry)(void), void *arg, void **stack_out) {
    return create_thread_prio(entry, arg, 44, stack_out);
}

static uint32_t wait_one(uint32_t handle, uint64_t timeout) {
    uint32_t h = handle;
    return (uint32_t)call(0x18, 0, (uint64_t)&h, 1, timeout).x[0];
}

TEST(svc_sleep_thread_sleeps) {
    init_once();
    uint64_t t0 = now_ns();
    call(0x0B, 2000000, 0, 0, 0);
    uint64_t dt = now_ns() - t0;
    ASSERT_TRUE(dt >= 2000000);
    ASSERT_TRUE(dt < 2000000000ull);
}

TEST(svc_get_system_tick_advances) {
    init_once();
    hos_svc_regs a = call(0x1E, 0, 0, 0, 0);
    call(0x0B, 100000, 0, 0, 0);
    hos_svc_regs b = call(0x1E, 0, 0, 0, 0);
    ASSERT_TRUE(b.x[0] > a.x[0]);
}

TEST(svc_get_info_values) {
    init_once();
    hos_svc_regs r = call(0x29, 0, 4, 0xFFFF8001, 0); /* HeapRegionAddress */
    ASSERT_EQ(r.x[0], HOS_RESULT_SUCCESS);
    ASSERT_EQ(r.x[1], HEAP_BASE);

    r = call(0x29, 0, 11, 0, 0); /* RandomEntropy[0] */
    ASSERT_EQ(r.x[0], HOS_RESULT_SUCCESS);
    hos_svc_regs r2 = call(0x29, 0, 11, 0, 1);
    ASSERT_NE(r.x[1], r2.x[1]);

    r = call(0x29, 0, 11, 0, 4);
    ASSERT_EQ(r.x[0], HOS_KERNEL_INVALID_COMBINATION);

    r = call(0x29, 0, 18, 0xFFFF8001, 0); /* ProgramId, from hos_cpu_params (hosrun --program-id) */
    ASSERT_EQ(r.x[0], HOS_RESULT_SUCCESS);
    ASSERT_EQ(r.x[1], 0x0100000000010000ull);

    r = call(0x29, 0, 9999, 0, 0);
    ASSERT_EQ(r.x[0], HOS_KERNEL_INVALID_ENUM_VALUE);
}

TEST(svc_heap_grow_query_shrink) {
    init_once();
    hos_svc_regs r = call(0x01, 0, 0x200000, 0, 0);
    ASSERT_EQ(r.x[0], HOS_RESULT_SUCCESS);
    ASSERT_EQ(r.x[1], HEAP_BASE);

    volatile uint64_t *p = (volatile uint64_t *)HEAP_BASE;
    p[0] = 0x1122334455667788ull;
    ASSERT_EQ(p[0], 0x1122334455667788ull);

    hos_memory_info info;
    r = call(0x06, (uint64_t)&info, 0, HEAP_BASE + 0x1000, 0);
    ASSERT_EQ(r.x[0], HOS_RESULT_SUCCESS);
    ASSERT_EQ(r.x[1], 0);
    ASSERT_EQ(info.addr, HEAP_BASE);
    ASSERT_EQ(info.size, 0x200000);
    ASSERT_EQ(info.state, HOS_MEM_NORMAL);
    ASSERT_EQ(info.perm, HOS_PERM_RW);

    call(0x06, (uint64_t)&info, 0, HEAP_BASE + 0x200000, 0);
    ASSERT_EQ(info.state, HOS_MEM_FREE);
    ASSERT_EQ(info.addr, HEAP_BASE + 0x200000);

    r = call(0x01, 0, 0x300000, 0, 0); /* not 2 MiB aligned */
    ASSERT_EQ(r.x[0], HOS_KERNEL_INVALID_SIZE);

    r = call(0x01, 0, 0, 0, 0);
    ASSERT_EQ(r.x[0], HOS_RESULT_SUCCESS);
    call(0x06, (uint64_t)&info, 0, HEAP_BASE, 0);
    ASSERT_EQ(info.state, HOS_MEM_FREE);
}

TEST(svc_map_unmap_memory_moves_pages) {
    init_once();
    hos_svc_regs r = call(0x01, 0, 0x400000, 0, 0);
    ASSERT_EQ(r.x[0], HOS_RESULT_SUCCESS);

    uint64_t src = HEAP_BASE + 0x200000;
    uint64_t dst = STACK_REGION + 0x10000;
    *(volatile uint64_t *)src = 0xABCD;

    r = call(0x04, dst, src, 0x1000, 0);
    ASSERT_EQ(r.x[0], HOS_RESULT_SUCCESS);
    ASSERT_EQ(*(volatile uint64_t *)dst, 0xABCD);

    hos_memory_info info;
    call(0x06, (uint64_t)&info, 0, src, 0);
    ASSERT_EQ(info.perm, HOS_PERM_NONE);
    ASSERT_TRUE(info.attr & HOS_ATTR_LOCKED);
    call(0x06, (uint64_t)&info, 0, dst, 0);
    ASSERT_EQ(info.state, HOS_MEM_STACK);
    ASSERT_EQ(info.perm, HOS_PERM_RW);

    r = call(0x05, dst, src, 0x1000, 0);
    ASSERT_EQ(r.x[0], HOS_RESULT_SUCCESS);
    ASSERT_EQ(*(volatile uint64_t *)src, 0xABCD);
    call(0x06, (uint64_t)&info, 0, src, 0);
    ASSERT_EQ(info.perm, HOS_PERM_RW);
    call(0x06, (uint64_t)&info, 0, dst, 0);
    ASSERT_EQ(info.state, HOS_MEM_FREE);

    r = call(0x04, HEAP_BASE, src, 0x1000, 0); /* dst outside stack/alias */
    ASSERT_EQ(r.x[0], HOS_KERNEL_INVALID_MEMORY_REGION);

    call(0x01, 0, 0, 0, 0);
}

TEST(svc_thread_create_start_exit) {
    init_once();
    volatile uint64_t flag = 0;
    void *stack;
    uint32_t handle = create_thread(test_thread_entry, (void *)&flag, &stack);
    ASSERT_TRUE(handle != 0);

    /* Not started yet: nothing happens. */
    call(0x0B, 20000000, 0, 0, 0);
    ASSERT_EQ(flag, 0);
    ASSERT_EQ(wait_one(handle, 0), HOS_KERNEL_TIMED_OUT);

    hos_svc_regs r = call(0x09, handle, 0, 0, 0);
    ASSERT_EQ(r.x[0], HOS_RESULT_SUCCESS);

    /* Thread exit is waitable, and stays signaled. */
    ASSERT_EQ(wait_one(handle, 5000000000ull), HOS_RESULT_SUCCESS);
    ASSERT_EQ(flag, 1);
    ASSERT_EQ(wait_one(handle, 0), HOS_RESULT_SUCCESS);

    r = call(0x16, handle, 0, 0, 0);
    ASSERT_EQ(r.x[0], HOS_RESULT_SUCCESS);
    r = call(0x16, handle, 0, 0, 0);
    ASSERT_EQ(r.x[0], HOS_KERNEL_INVALID_HANDLE);
    munmap(stack, 0x10000);
}

TEST(svc_mutex_contention) {
    init_once();
    static uint32_t mutex;
    static uint32_t counter;
    mutex = 0;
    counter = 0;
    enum { N = 4 };
    struct {
        uint32_t *mutex;
        uint32_t self;
        uint32_t *counter;
    } args[N];
    uint32_t handles[N];
    void *stacks[N];
    for (int i = 0; i < N; i++) {
        args[i].mutex = &mutex;
        args[i].counter = &counter;
        handles[i] = create_thread(test_mutex_entry, &args[i], &stacks[i]);
        ASSERT_TRUE(handles[i] != 0);
        args[i].self = handles[i];
    }
    for (int i = 0; i < N; i++) {
        ASSERT_EQ(call(0x09, handles[i], 0, 0, 0).x[0], HOS_RESULT_SUCCESS);
    }
    for (int i = 0; i < N; i++) {
        ASSERT_EQ(wait_one(handles[i], 5000000000ull), HOS_RESULT_SUCCESS);
        call(0x16, handles[i], 0, 0, 0);
        munmap(stacks[i], 0x10000);
    }
    ASSERT_EQ(counter, N);
    ASSERT_EQ(mutex, 0);
}

TEST(svc_arbitrate_lock_fast_paths) {
    init_once();
    static uint32_t mutex;
    mutex = 0x300; /* owner without listeners flag: nothing to wait for */
    hos_svc_regs r = call(0x1A, 0x300, (uint64_t)&mutex, 0x301, 0);
    ASSERT_EQ(r.x[0], HOS_RESULT_SUCCESS);
    r = call(0x1A, 0x300, (uint64_t)&mutex + 1, 0x301, 0);
    ASSERT_EQ(r.x[0], HOS_KERNEL_INVALID_ADDRESS);
    mutex = 0x300 | MUTEX_FLAG;
    r = call(0x1B, (uint64_t)&mutex, 0, 0, 0); /* no waiters: cleared */
    ASSERT_EQ(r.x[0], HOS_RESULT_SUCCESS);
    ASSERT_EQ(mutex, 0);
}

TEST(svc_condvar_timeout_and_signal_clears_key) {
    init_once();
    static uint32_t mutex, cv;
    uint32_t self = hos_cpu_adopt_current_thread();
    mutex = self;
    cv = 1;
    uint64_t t0 = now_ns();
    hos_svc_regs r = call(0x1C, (uint64_t)&mutex, (uint64_t)&cv, self, 5000000);
    ASSERT_EQ(r.x[0], HOS_KERNEL_TIMED_OUT);
    ASSERT_TRUE(now_ns() - t0 >= 5000000);
    ASSERT_EQ(mutex, 0); /* released on entry, not re-acquired on timeout */

    r = call(0x1D, (uint64_t)&cv, (uint64_t)(int64_t)-1, 0, 0);
    ASSERT_EQ(r.x[0], HOS_RESULT_SUCCESS);
    ASSERT_EQ(cv, 0);
}

/* ---- wake order --------------------------------------------------------------- */

typedef struct wake_args {
    uint64_t key;
    uint64_t order;
    uint64_t count;
    uint64_t id;
    uint64_t flag;
    uint64_t self;
    uint64_t mutex;
} wake_args;

enum { WAKE_N = 4 };

/* Four threads park on `key` one after another, so arrival order is known:
 * A (prio 50), B (30), C (40), D (30). The kernel wakes by priority, then
 * arrival: B, D, C, A. */
static const int32_t k_wake_prio[WAKE_N] = {50, 30, 40, 30};

typedef struct wake_fixture {
    wake_args args[WAKE_N];
    uint32_t handles[WAKE_N];
    void *stacks[WAKE_N];
    uint32_t order[WAKE_N];
    uint32_t count;
    uint32_t flags[WAKE_N];
    uint32_t mutexes[WAKE_N]; /* the condvar body's own mutex per thread */
} wake_fixture;

static int poll_u32(volatile uint32_t *p, uint32_t want) {
    for (int i = 0; i < 5000; i++) {
        if (__atomic_load_n(p, __ATOMIC_ACQUIRE) == want) {
            return 1;
        }
        call(0x0B, 1000000, 0, 0, 0);
    }
    return 0;
}

/* Wait until `n` threads are parked on `key` (hos_cpu's test hook). */
static int poll_parked(volatile void *key, int32_t n) {
    for (int i = 0; i < 5000; i++) {
        if (hos_sync_waiters_on((uint64_t)key) == n) {
            return 1;
        }
        call(0x0B, 1000000, 0, 0, 0);
    }
    return 0;
}

/* After a failed start: release whatever parked on `key` so the threads can exit. */
static void wake_fixture_abort(wake_fixture *f, volatile void *key, volatile uint32_t *mutex) {
    call(0x35, (uint64_t)key, 0, 0, (uint64_t)(uint32_t)-1); /* SignalToAddress, all */
    call(0x1D, (uint64_t)key, (uint64_t)(uint32_t)-1, 0, 0); /* SignalProcessWideKey, all */
    for (int i = 0; i < WAKE_N; i++) {
        call(0x1B, (uint64_t)&f->mutexes[i], 0, 0, 0); /* ArbitrateUnlock the own mutexes */
    }
    if (mutex) {
        call(0x1B, (uint64_t)mutex, 0, 0, 0);
    }
    for (int i = 0; i < WAKE_N; i++) {
        if (f->handles[i]) {
            wait_one(f->handles[i], 1000000000ull);
            call(0x16, f->handles[i], 0, 0, 0);
        }
    }
}

/* Create and start the threads one at a time; each is parked on `key` before the
 * next starts, so arrival order is known. `mutex`, when given, is the condvar
 * body's mutex, shared by all four and held by the test. */
static int wake_fixture_start_on(wake_fixture *f, void (*entry)(void), volatile void *key,
                                 volatile uint32_t *mutex) {
    memset(f, 0, sizeof(*f));
    for (int i = 0; i < WAKE_N; i++) {
        f->args[i].key = (uint64_t)key;
        f->args[i].order = (uint64_t)f->order;
        f->args[i].count = (uint64_t)&f->count;
        f->args[i].id = (uint64_t)(i + 1);
        f->args[i].flag = (uint64_t)&f->flags[i];
        f->args[i].mutex = (uint64_t)(mutex ? mutex : &f->mutexes[i]);
        f->handles[i] = create_thread_prio(entry, &f->args[i], k_wake_prio[i], &f->stacks[i]);
        if (!f->handles[i]) {
            wake_fixture_abort(f, key, mutex);
            return 0;
        }
        f->args[i].self = f->handles[i];
        f->mutexes[i] = f->handles[i];
    }
    for (int i = 0; i < WAKE_N; i++) {
        if (call(0x09, f->handles[i], 0, 0, 0).x[0] != HOS_RESULT_SUCCESS ||
            !poll_parked(key, i + 1)) {
            wake_fixture_abort(f, key, mutex);
            return 0;
        }
    }
    return 1;
}

static int wake_fixture_start(wake_fixture *f, void (*entry)(void), volatile void *key) {
    return wake_fixture_start_on(f, entry, key, NULL);
}

static int wake_fixture_finish(wake_fixture *f) {
    int ok = 1;
    for (int i = 0; i < WAKE_N; i++) {
        ok = ok && wait_one(f->handles[i], 5000000000ull) == HOS_RESULT_SUCCESS;
        call(0x16, f->handles[i], 0, 0, 0);
        munmap(f->stacks[i], 0x10000);
    }
    return ok;
}

TEST(svc_wake_order_address_arbiter) {
    init_once();
    static wake_fixture f;
    static volatile int32_t word;
    word = 0;
    ASSERT_TRUE(wake_fixture_start(&f, test_wake_arbiter_entry, &word));
    /* A parked thread whose priority changes re-queues behind its new peers. */
    ASSERT_EQ(call(0x0D, f.handles[0], 30, 0, 0).x[0], HOS_RESULT_SUCCESS);
    for (uint32_t n = 1; n <= WAKE_N; n++) {
        ASSERT_EQ(call(0x35, (uint64_t)&word, 0, 0, 1).x[0], HOS_RESULT_SUCCESS); /* one */
        ASSERT_TRUE(poll_u32(&f.count, n));
    }
    ASSERT_TRUE(wake_fixture_finish(&f));
    ASSERT_EQ(f.order[0], 2); /* B, 30 */
    ASSERT_EQ(f.order[1], 4); /* D, 30 */
    ASSERT_EQ(f.order[2], 1); /* A, now 30, behind B and D */
    ASSERT_EQ(f.order[3], 3); /* C, 40 */
}

TEST(svc_wake_order_raised_priority_goes_first) {
    init_once();
    static wake_fixture f;
    static volatile int32_t word;
    word = 0;
    ASSERT_TRUE(wake_fixture_start(&f, test_wake_arbiter_entry, &word));
    ASSERT_EQ(call(0x0D, f.handles[2], 28, 0, 0).x[0], HOS_RESULT_SUCCESS); /* C to 28 */
    for (uint32_t n = 1; n <= WAKE_N; n++) {
        ASSERT_EQ(call(0x35, (uint64_t)&word, 0, 0, 1).x[0], HOS_RESULT_SUCCESS); /* one */
        ASSERT_TRUE(poll_u32(&f.count, n));
    }
    ASSERT_TRUE(wake_fixture_finish(&f));
    ASSERT_EQ(hos_sync_waiters_on((uint64_t)&word), 0);
    ASSERT_EQ(f.order[0], 3); /* C, 28 */
    ASSERT_EQ(f.order[1], 2); /* B, 30 */
    ASSERT_EQ(f.order[2], 4); /* D, 30 */
    ASSERT_EQ(f.order[3], 1); /* A, 50 */
}

TEST(svc_wake_order_mutex_handoff) {
    init_once();
    static wake_fixture f;
    static volatile uint32_t mutex;
    mutex = hos_cpu_adopt_current_thread(); /* held by the test thread */
    ASSERT_TRUE(wake_fixture_start(&f, test_wake_mutex_entry, &mutex));
    ASSERT_EQ(mutex & MUTEX_FLAG, MUTEX_FLAG);
    ASSERT_EQ(call(0x1B, (uint64_t)&mutex, 0, 0, 0).x[0], HOS_RESULT_SUCCESS);
    ASSERT_TRUE(poll_u32(&f.count, WAKE_N));
    ASSERT_TRUE(wake_fixture_finish(&f));
    ASSERT_EQ(mutex, 0);
    ASSERT_EQ(f.order[0], 2);
    ASSERT_EQ(f.order[1], 4);
    ASSERT_EQ(f.order[2], 3);
    ASSERT_EQ(f.order[3], 1);
}

TEST(svc_wake_order_condvar) {
    init_once();
    static wake_fixture f;
    static volatile uint32_t cv;
    cv = 1;
    ASSERT_TRUE(wake_fixture_start(&f, test_wake_condvar_entry, &cv));
    for (int i = 0; i < WAKE_N; i++) {
        ASSERT_EQ(f.mutexes[i], 0); /* released on entry to the wait */
    }
    for (uint32_t n = 1; n <= WAKE_N; n++) {
        ASSERT_EQ(call(0x1D, (uint64_t)&cv, 1, 0, 0).x[0], HOS_RESULT_SUCCESS);
        ASSERT_TRUE(poll_u32(&f.count, n));
    }
    ASSERT_EQ(cv, 0); /* key cleared once the queue is empty */
    ASSERT_TRUE(wake_fixture_finish(&f));
    for (int i = 0; i < WAKE_N; i++) {
        ASSERT_EQ(f.mutexes[i], f.handles[i]); /* handed back to the woken thread */
    }
    ASSERT_EQ(f.order[0], 2);
    ASSERT_EQ(f.order[1], 4);
    ASSERT_EQ(f.order[2], 3);
    ASSERT_EQ(f.order[3], 1);
}

/* Condvar waiters whose mutex is held move onto it in priority order, and one
 * unlock drains the chain; signalling everyone clears the key. */
TEST(svc_wake_order_condvar_onto_contended_mutex) {
    init_once();
    static wake_fixture f;
    static volatile uint32_t cv, mutex;
    cv = 1;
    mutex = 0;
    ASSERT_TRUE(wake_fixture_start_on(&f, test_wake_condvar_mutex_entry, &cv, &mutex));
    ASSERT_EQ(mutex, 0);                    /* each wait released it */
    mutex = hos_cpu_adopt_current_thread(); /* now held by the test thread */
    ASSERT_EQ(call(0x1D, (uint64_t)&cv, (uint64_t)(uint32_t)-1, 0, 0).x[0], HOS_RESULT_SUCCESS);
    ASSERT_TRUE(poll_parked(&mutex, WAKE_N)); /* all moved onto the mutex */
    ASSERT_EQ(cv, 0);
    ASSERT_EQ(mutex & MUTEX_FLAG, MUTEX_FLAG);
    ASSERT_EQ(f.count, 0);
    ASSERT_EQ(call(0x1B, (uint64_t)&mutex, 0, 0, 0).x[0], HOS_RESULT_SUCCESS);
    ASSERT_TRUE(poll_u32(&f.count, WAKE_N));
    ASSERT_TRUE(wake_fixture_finish(&f));
    ASSERT_EQ(mutex, 0);
    ASSERT_EQ(f.order[0], 2);
    ASSERT_EQ(f.order[1], 4);
    ASSERT_EQ(f.order[2], 3);
    ASSERT_EQ(f.order[3], 1);
}

/* SignalAndModifyByWaitingCountIfEqual (type 2) writes what the kernel would:
 * one branch per test step. The word is 0 and `value` is 0 throughout. */
static hos_svc_regs signal_modify(volatile int32_t *word, int32_t count) {
    return call(0x35, (uint64_t)word, 2, 0, (uint64_t)(uint32_t)count);
}

TEST(svc_signal_modify_by_waiting_count_with_waiters) {
    init_once();
    static wake_fixture f;
    static volatile int32_t word;
    word = 0;
    ASSERT_TRUE(wake_fixture_start(&f, test_wake_arbiter_entry, &word));
    /* 4 waiting, count 1: three beyond the first, more than count: unchanged, one woken */
    ASSERT_EQ(signal_modify(&word, 1).x[0], HOS_RESULT_SUCCESS);
    ASSERT_EQ(word, 0);
    ASSERT_TRUE(poll_u32(&f.count, 1));
    word = 0;
    /* 3 waiting, count 4: two beyond the first, within count: down by one, all woken */
    ASSERT_EQ(signal_modify(&word, 4).x[0], HOS_RESULT_SUCCESS);
    ASSERT_EQ(word, -1);
    ASSERT_TRUE(poll_u32(&f.count, WAKE_N));
    ASSERT_TRUE(wake_fixture_finish(&f));
    word = 0;
    /* nobody waiting: up by one, whatever the count */
    ASSERT_EQ(signal_modify(&word, 1).x[0], HOS_RESULT_SUCCESS);
    ASSERT_EQ(word, 1);
    word = 0;
    ASSERT_EQ(signal_modify(&word, 0).x[0], HOS_RESULT_SUCCESS);
    ASSERT_EQ(word, 1);
    /* the value must match */
    ASSERT_EQ(signal_modify(&word, 1).x[0], HOS_KERNEL_INVALID_STATE);
}

TEST(svc_signal_modify_by_waiting_count_all_and_single) {
    init_once();
    static wake_fixture f;
    static volatile int32_t word;
    word = 0;
    ASSERT_TRUE(wake_fixture_start(&f, test_wake_arbiter_entry, &word));
    /* count <= 0 with waiters: down by two, everyone woken */
    ASSERT_EQ(signal_modify(&word, -1).x[0], HOS_RESULT_SUCCESS);
    ASSERT_EQ(word, -2);
    ASSERT_TRUE(poll_u32(&f.count, WAKE_N));
    ASSERT_TRUE(wake_fixture_finish(&f));
    ASSERT_EQ(hos_sync_waiters_on((uint64_t)&word), 0);
    word = 0;
    ASSERT_TRUE(wake_fixture_start(&f, test_wake_arbiter_entry, &word));
    /* leave one waiter (plain signal of three), then count 1: none beyond it, up by one */
    ASSERT_EQ(call(0x35, (uint64_t)&word, 0, 0, 3).x[0], HOS_RESULT_SUCCESS);
    ASSERT_TRUE(poll_u32(&f.count, 3));
    ASSERT_TRUE(poll_parked(&word, 1));
    ASSERT_EQ(signal_modify(&word, 1).x[0], HOS_RESULT_SUCCESS);
    ASSERT_EQ(word, 1);
    ASSERT_TRUE(poll_u32(&f.count, WAKE_N));
    ASSERT_TRUE(wake_fixture_finish(&f));
}

TEST(svc_address_arbiter_fast_paths) {
    init_once();
    static int32_t word;
    word = 5;
    hos_svc_regs r = call(0x34, (uint64_t)&word, 0, 3, 0); /* WaitIfLessThan 3: 5 < 3 false */
    ASSERT_EQ(r.x[0], HOS_KERNEL_INVALID_STATE);
    r = call(0x34, (uint64_t)&word, 2, 5, 0); /* WaitIfEqual, poll */
    ASSERT_EQ(r.x[0], HOS_KERNEL_TIMED_OUT);
    r = call(0x34, (uint64_t)&word, 1, 10, 0); /* DecrementAndWaitIfLessThan */
    ASSERT_EQ(r.x[0], HOS_KERNEL_TIMED_OUT);
    ASSERT_EQ(word, 4);
    r = call(0x35, (uint64_t)&word, 1, 4, 1); /* SignalAndIncrementIfEqual */
    ASSERT_EQ(r.x[0], HOS_RESULT_SUCCESS);
    ASSERT_EQ(word, 5);
    r = call(0x35, (uint64_t)&word, 1, 4, 1);
    ASSERT_EQ(r.x[0], HOS_KERNEL_INVALID_STATE);
    r = call(0x35, (uint64_t)&word, 2, 5, 1); /* no waiters: value + 1 */
    ASSERT_EQ(r.x[0], HOS_RESULT_SUCCESS);
    ASSERT_EQ(word, 6);
}

TEST(svc_events) {
    init_once();
    hos_svc_regs r = call(0x45, 0, 0, 0, 0);
    ASSERT_EQ(r.x[0], HOS_RESULT_SUCCESS);
    uint32_t w = (uint32_t)r.x[1], rd = (uint32_t)r.x[2];
    ASSERT_TRUE(w && rd && w != rd);

    ASSERT_EQ(wait_one(rd, 0), HOS_KERNEL_TIMED_OUT);
    ASSERT_EQ(wait_one(w, 0), HOS_KERNEL_INVALID_HANDLE); /* writable side is not waitable */
    ASSERT_EQ(call(0x17, rd, 0, 0, 0).x[0], HOS_KERNEL_INVALID_STATE); /* ResetSignal unsignaled */

    ASSERT_EQ(call(0x11, w, 0, 0, 0).x[0], HOS_RESULT_SUCCESS);
    ASSERT_EQ(wait_one(rd, 0), HOS_RESULT_SUCCESS);
    ASSERT_EQ(call(0x17, rd, 0, 0, 0).x[0], HOS_RESULT_SUCCESS);
    ASSERT_EQ(wait_one(rd, 0), HOS_KERNEL_TIMED_OUT);

    ASSERT_EQ(call(0x11, w, 0, 0, 0).x[0], HOS_RESULT_SUCCESS);
    ASSERT_EQ(call(0x12, rd, 0, 0, 0).x[0], HOS_RESULT_SUCCESS); /* ClearEvent */
    uint64_t t0 = now_ns();
    ASSERT_EQ(wait_one(rd, 3000000), HOS_KERNEL_TIMED_OUT);
    ASSERT_TRUE(now_ns() - t0 >= 3000000);

    /* CancelSynchronization with no waiter is remembered for the next wait. */
    uint32_t self = hos_cpu_adopt_current_thread();
    ASSERT_EQ(call(0x19, self, 0, 0, 0).x[0], HOS_RESULT_SUCCESS);
    ASSERT_EQ(wait_one(rd, 1000000000ull), HOS_KERNEL_CANCELLED);

    ASSERT_EQ(call(0x16, w, 0, 0, 0).x[0], HOS_RESULT_SUCCESS);
    ASSERT_EQ(call(0x16, rd, 0, 0, 0).x[0], HOS_RESULT_SUCCESS);
    ASSERT_EQ(call(0x11, w, 0, 0, 0).x[0], HOS_KERNEL_INVALID_HANDLE);
}

TEST(svc_handles_and_misc) {
    init_once();
    hos_svc_regs r = call(0x16, 0xDEAD, 0, 0, 0);
    ASSERT_EQ(r.x[0], HOS_KERNEL_INVALID_HANDLE);

    r = call(0x24, 0, 0xFFFF8001, 0, 0);
    ASSERT_EQ(r.x[0], HOS_RESULT_SUCCESS);
    ASSERT_TRUE(r.x[1] > 0);

    r = call(0x27, (uint64_t) "test string\n", 12, 0, 0);
    ASSERT_EQ(r.x[0], HOS_RESULT_SUCCESS);

    r = call(0x51, 0, 0, 0, 0); /* MapTransferMemory: not yet */
    ASSERT_EQ(r.x[0], HOS_KERNEL_NOT_IMPLEMENTED);

    r = call(0x10, 0, 0, 0, 0);
    ASSERT_TRUE(r.x[0] < 4);
}

TEST(svc_shared_memory_create_map_unmap) {
    init_once();
    hos_svc_regs r = call(0x50, 0, 0x2000, 3, 1); /* CreateSharedMemory(size, rw, r) */
    ASSERT_EQ(r.x[0], HOS_RESULT_SUCCESS);
    uint32_t h = (uint32_t)r.x[1];
    ASSERT_TRUE(h != 0);

    r = call(0x50, 0, 0x1234, 3, 1);
    ASSERT_EQ(r.x[0], HOS_KERNEL_INVALID_SIZE);

    uint64_t a = STACK_REGION + 0x100000;
    uint64_t b = STACK_REGION + 0x200000;
    r = call(0x13, h, a, 0x2000, 3);
    ASSERT_EQ(r.x[0], HOS_RESULT_SUCCESS);
    r = call(0x13, h, b, 0x2000, 1);
    ASSERT_EQ(r.x[0], HOS_RESULT_SUCCESS);
    *(volatile uint32_t *)a = 0xC0FFEE;
    ASSERT_EQ(*(volatile uint32_t *)b, 0xC0FFEE); /* both mappings alias the same pages */

    hos_memory_info info;
    call(0x06, (uint64_t)&info, 0, a, 0);
    ASSERT_EQ(info.state, HOS_MEM_SHARED);
    ASSERT_EQ(info.perm, HOS_PERM_RW);

    r = call(0x13, h, a + 0x10000, 0x1000, 3); /* wrong size */
    ASSERT_EQ(r.x[0], HOS_KERNEL_INVALID_SIZE);

    r = call(0x14, h, a, 0x2000, 0);
    ASSERT_EQ(r.x[0], HOS_RESULT_SUCCESS);
    r = call(0x14, h, b, 0x2000, 0);
    ASSERT_EQ(r.x[0], HOS_RESULT_SUCCESS);
    call(0x06, (uint64_t)&info, 0, a, 0);
    ASSERT_EQ(info.state, HOS_MEM_FREE);
    ASSERT_EQ(call(0x16, h, 0, 0, 0).x[0], HOS_RESULT_SUCCESS);
    ASSERT_EQ(call(0x13, h, a, 0x2000, 3).x[0], HOS_KERNEL_INVALID_HANDLE);
}

TEST(svc_transfer_memory_create_close) {
    init_once();
    hos_svc_regs r = call(0x01, 0, 0x200000, 0, 0);
    ASSERT_EQ(r.x[0], HOS_RESULT_SUCCESS);
    r = call(0x15, 0, HEAP_BASE, 0x1000, 0); /* CreateTransferMemory(addr, size, perm none) */
    ASSERT_EQ(r.x[0], HOS_RESULT_SUCCESS);
    uint32_t h = (uint32_t)r.x[1];
    hos_memory_info info;
    call(0x06, (uint64_t)&info, 0, HEAP_BASE, 0);
    ASSERT_TRUE(info.attr & HOS_ATTR_LOCKED);
    ASSERT_EQ(call(0x16, h, 0, 0, 0).x[0], HOS_RESULT_SUCCESS);
    call(0x06, (uint64_t)&info, 0, HEAP_BASE, 0);
    ASSERT_EQ(info.attr & HOS_ATTR_LOCKED, 0);
    call(0x01, 0, 0, 0, 0);
}

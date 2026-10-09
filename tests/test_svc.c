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

static uint32_t create_thread(void (*entry)(void), void *arg, void **stack_out) {
    size_t stack_size = 0x10000;
    uint8_t *stack =
        mmap(NULL, stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack == MAP_FAILED) {
        return 0;
    }
    *stack_out = stack;
    hos_svc_regs r = {
        {0, (uint64_t)entry, (uint64_t)arg, (uint64_t)stack + stack_size, 44, 0, 0, 0}};
    hos_svc_dispatch(0x08, &r);
    return r.x[0] == 0 ? (uint32_t)r.x[1] : 0;
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

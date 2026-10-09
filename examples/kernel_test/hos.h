#ifndef EXAMPLE_HOS_H
#define EXAMPLE_HOS_H

/*
 * Freestanding HOS user-mode helpers for test modules: svc wrappers, a
 * libnx-style mutex on top of the address arbiter, and tiny string helpers.
 * No libc. Everything is static inline so the module stays one object.
 */

#include <stddef.h>
#include <stdint.h>

#define HOS_MUTEX_HAS_LISTENERS 0x40000000u
#define HOS_INFINITE            0xFFFFFFFFFFFFFFFFull

#define SVC_CLOBBERS "x4", "x5", "x6", "x7", "memory"

static inline uint32_t svc_set_heap_size(uint64_t size, void **out) {
    register uint64_t x0 __asm__("x0");
    register uint64_t x1 __asm__("x1") = size;
    __asm__ volatile("svc #0x01" : "=r"(x0), "+r"(x1) : : "x2", "x3", SVC_CLOBBERS);
    *out = (void *)x1;
    return (uint32_t)x0;
}

static inline uint32_t svc_create_thread(uint32_t *out, void (*entry)(void *), void *arg,
                                         void *stack_top, int prio, int core) {
    register uint64_t x0 __asm__("x0");
    register uint64_t x1 __asm__("x1") = (uint64_t)entry;
    register uint64_t x2 __asm__("x2") = (uint64_t)arg;
    register uint64_t x3 __asm__("x3") = (uint64_t)stack_top;
    register uint64_t x4 __asm__("x4") = (uint64_t)prio;
    register uint64_t x5 __asm__("x5") = (uint64_t)core;
    __asm__ volatile("svc #0x08"
                     : "=r"(x0), "+r"(x1)
                     : "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                     : "x6", "x7", "memory");
    *out = (uint32_t)x1;
    return (uint32_t)x0;
}

static inline uint32_t svc_start_thread(uint32_t h) {
    register uint64_t x0 __asm__("x0") = h;
    __asm__ volatile("svc #0x09" : "+r"(x0) : : "x1", "x2", "x3", SVC_CLOBBERS);
    return (uint32_t)x0;
}

static inline void svc_exit_thread(void) {
    __asm__ volatile("svc #0x0A" : : : "memory");
    for (;;) {
    }
}

static inline void svc_sleep_thread(int64_t ns) {
    register uint64_t x0 __asm__("x0") = (uint64_t)ns;
    __asm__ volatile("svc #0x0B" : "+r"(x0) : : "x1", "x2", "x3", SVC_CLOBBERS);
}

static inline uint32_t svc_signal_event(uint32_t h) {
    register uint64_t x0 __asm__("x0") = h;
    __asm__ volatile("svc #0x11" : "+r"(x0) : : "x1", "x2", "x3", SVC_CLOBBERS);
    return (uint32_t)x0;
}

static inline uint32_t svc_clear_event(uint32_t h) {
    register uint64_t x0 __asm__("x0") = h;
    __asm__ volatile("svc #0x12" : "+r"(x0) : : "x1", "x2", "x3", SVC_CLOBBERS);
    return (uint32_t)x0;
}

static inline uint32_t svc_close_handle(uint32_t h) {
    register uint64_t x0 __asm__("x0") = h;
    __asm__ volatile("svc #0x16" : "+r"(x0) : : "x1", "x2", "x3", SVC_CLOBBERS);
    return (uint32_t)x0;
}

static inline uint32_t svc_wait_synchronization(int32_t *index, const uint32_t *handles,
                                                int32_t count, uint64_t timeout) {
    register uint64_t x0 __asm__("x0");
    register uint64_t x1 __asm__("x1") = (uint64_t)handles;
    register uint64_t x2 __asm__("x2") = (uint64_t)count;
    register uint64_t x3 __asm__("x3") = timeout;
    __asm__ volatile("svc #0x18" : "=r"(x0), "+r"(x1) : "r"(x2), "r"(x3) : SVC_CLOBBERS);
    *index = (int32_t)x1;
    return (uint32_t)x0;
}

static inline uint32_t svc_arbitrate_lock(uint32_t owner, uint32_t *mutex, uint32_t self) {
    register uint64_t x0 __asm__("x0") = owner;
    register uint64_t x1 __asm__("x1") = (uint64_t)mutex;
    register uint64_t x2 __asm__("x2") = self;
    __asm__ volatile("svc #0x1A" : "+r"(x0) : "r"(x1), "r"(x2) : "x3", SVC_CLOBBERS);
    return (uint32_t)x0;
}

static inline uint32_t svc_arbitrate_unlock(uint32_t *mutex) {
    register uint64_t x0 __asm__("x0") = (uint64_t)mutex;
    __asm__ volatile("svc #0x1B" : "+r"(x0) : : "x1", "x2", "x3", SVC_CLOBBERS);
    return (uint32_t)x0;
}

static inline uint32_t svc_wait_process_wide_key_atomic(uint32_t *mutex, uint32_t *cv,
                                                        uint32_t self, uint64_t timeout) {
    register uint64_t x0 __asm__("x0") = (uint64_t)mutex;
    register uint64_t x1 __asm__("x1") = (uint64_t)cv;
    register uint64_t x2 __asm__("x2") = self;
    register uint64_t x3 __asm__("x3") = timeout;
    __asm__ volatile("svc #0x1C" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : SVC_CLOBBERS);
    return (uint32_t)x0;
}

static inline uint32_t svc_signal_process_wide_key(uint32_t *cv, int32_t count) {
    register uint64_t x0 __asm__("x0") = (uint64_t)cv;
    register uint64_t x1 __asm__("x1") = (uint64_t)count;
    __asm__ volatile("svc #0x1D" : "+r"(x0) : "r"(x1) : "x2", "x3", SVC_CLOBBERS);
    return (uint32_t)x0;
}

static inline uint64_t svc_get_system_tick(void) {
    register uint64_t x0 __asm__("x0");
    __asm__ volatile("svc #0x1E" : "=r"(x0) : : "x1", "x2", "x3", SVC_CLOBBERS);
    return x0;
}

static inline uint32_t svc_connect_to_named_port(uint32_t *out, const char *name) {
    register uint64_t x0 __asm__("x0");
    register uint64_t x1 __asm__("x1") = (uint64_t)name;
    __asm__ volatile("svc #0x1F" : "=r"(x0), "+r"(x1) : : "x2", "x3", SVC_CLOBBERS);
    *out = (uint32_t)x1;
    return (uint32_t)x0;
}

static inline uint32_t svc_send_sync_request(uint32_t session) {
    register uint64_t x0 __asm__("x0") = session;
    __asm__ volatile("svc #0x21" : "+r"(x0) : : "x1", "x2", "x3", SVC_CLOBBERS);
    return (uint32_t)x0;
}

static inline void svc_break(uint32_t reason, uint64_t addr, uint64_t size) {
    register uint64_t x0 __asm__("x0") = reason;
    register uint64_t x1 __asm__("x1") = addr;
    register uint64_t x2 __asm__("x2") = size;
    __asm__ volatile("svc #0x26" : "+r"(x0) : "r"(x1), "r"(x2) : "x3", SVC_CLOBBERS);
}

static inline uint32_t svc_output_debug_string(const char *s, size_t len) {
    register uint64_t x0 __asm__("x0") = (uint64_t)s;
    register uint64_t x1 __asm__("x1") = len;
    __asm__ volatile("svc #0x27" : "+r"(x0) : "r"(x1) : "x2", "x3", SVC_CLOBBERS);
    return (uint32_t)x0;
}

static inline uint32_t svc_get_info(uint64_t *out, uint32_t id, uint32_t handle, uint64_t sub) {
    register uint64_t x0 __asm__("x0");
    register uint64_t x1 __asm__("x1") = id;
    register uint64_t x2 __asm__("x2") = handle;
    register uint64_t x3 __asm__("x3") = sub;
    __asm__ volatile("svc #0x29" : "=r"(x0), "+r"(x1) : "r"(x2), "r"(x3) : SVC_CLOBBERS);
    *out = x1;
    return (uint32_t)x0;
}

static inline uint32_t svc_wait_for_address(void *addr, uint32_t type, int32_t value,
                                            uint64_t timeout) {
    register uint64_t x0 __asm__("x0") = (uint64_t)addr;
    register uint64_t x1 __asm__("x1") = type;
    register uint64_t x2 __asm__("x2") = (uint64_t)(uint32_t)value;
    register uint64_t x3 __asm__("x3") = timeout;
    __asm__ volatile("svc #0x34" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : SVC_CLOBBERS);
    return (uint32_t)x0;
}

static inline uint32_t svc_signal_to_address(void *addr, uint32_t type, int32_t value,
                                             int32_t count) {
    register uint64_t x0 __asm__("x0") = (uint64_t)addr;
    register uint64_t x1 __asm__("x1") = type;
    register uint64_t x2 __asm__("x2") = (uint64_t)(uint32_t)value;
    register uint64_t x3 __asm__("x3") = (uint64_t)(uint32_t)count;
    __asm__ volatile("svc #0x35" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : SVC_CLOBBERS);
    return (uint32_t)x0;
}

static inline uint32_t svc_create_event(uint32_t *writable, uint32_t *readable) {
    register uint64_t x0 __asm__("x0");
    register uint64_t x1 __asm__("x1");
    register uint64_t x2 __asm__("x2");
    __asm__ volatile("svc #0x45" : "=r"(x0), "=r"(x1), "=r"(x2) : : "x3", SVC_CLOBBERS);
    *writable = (uint32_t)x1;
    *readable = (uint32_t)x2;
    return (uint32_t)x0;
}

static inline void svc_exit_process(void) {
    __asm__ volatile("svc #0x07" : : : "memory");
    for (;;) {
    }
}

/* ---- mutex, as libnx does it ------------------------------------------------- */

static inline void hos_mutex_lock(uint32_t *m, uint32_t self) {
    uint32_t cur = __atomic_load_n(m, __ATOMIC_SEQ_CST);
    for (;;) {
        if (cur == 0) {
            if (__atomic_compare_exchange_n(m, &cur, self, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
                return;
            }
            continue;
        }
        if ((cur & ~HOS_MUTEX_HAS_LISTENERS) == self) {
            return; /* already ours */
        }
        if (cur & HOS_MUTEX_HAS_LISTENERS) {
            svc_arbitrate_lock(cur & ~HOS_MUTEX_HAS_LISTENERS, m, self);
        } else if (__atomic_compare_exchange_n(m, &cur, cur | HOS_MUTEX_HAS_LISTENERS, 0,
                                               __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            svc_arbitrate_lock(cur, m, self);
        }
        cur = __atomic_load_n(m, __ATOMIC_SEQ_CST);
        if ((cur & ~HOS_MUTEX_HAS_LISTENERS) == self) {
            return;
        }
    }
}

static inline void hos_mutex_unlock(uint32_t *m, uint32_t self) {
    uint32_t cur = self;
    if (__atomic_compare_exchange_n(m, &cur, 0, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        return;
    }
    svc_arbitrate_unlock(m);
}

/* ---- strings ------------------------------------------------------------------ */

static inline size_t hos_strlen(const char *s) {
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static inline void hos_puts(const char *s) {
    svc_output_debug_string(s, hos_strlen(s));
}

/* Append a hex number; returns new end pointer. */
static inline char *hos_hex(char *p, uint64_t v) {
    static const char digits[] = "0123456789abcdef";
    char tmp[16];
    int n = 0;
    do {
        tmp[n++] = digits[v & 0xF];
        v >>= 4;
    } while (v);
    *p++ = '0';
    *p++ = 'x';
    while (n) {
        *p++ = tmp[--n];
    }
    *p = '\0';
    return p;
}

static inline char *hos_str(char *p, const char *s) {
    while (*s) {
        *p++ = *s++;
    }
    *p = '\0';
    return p;
}

#endif /* EXAMPLE_HOS_H */

/*
 * Synchronisation: the address arbiter (mutexes, condition variables,
 * WaitForAddress), events and WaitSynchronization. One lock, one wait list;
 * each blocked thread sleeps on its own condition variable. HOS wakes in
 * priority order; we wake FIFO, which no correct program can tell apart.
 */

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#include "common/hos_log.h"
#include "cpu_internal.h"

#define MOD        "sync"
#define EVENT_POOL 4096

typedef enum wait_kind {
    WAIT_MUTEX,   /* key = mutex address; woken by ArbitrateUnlock */
    WAIT_CONDVAR, /* key = condvar address; woken by SignalProcessWideKey */
    WAIT_ADDRESS, /* key = address; woken by SignalToAddress */
    WAIT_SYNC,    /* objs[]; woken by any signaled object */
} wait_kind;

typedef struct waiter {
    struct waiter *prev, *next;
    wait_kind kind;
    uint64_t key;
    uint64_t mutex_addr; /* WAIT_CONDVAR: mutex to re-acquire */
    hos_thread *thread;
    pthread_cond_t cond;
    int woken;
    hos_result result;
    int32_t index;
    void *objs[HOS_WAIT_MAX_HANDLES];
    hos_obj_type types[HOS_WAIT_MAX_HANDLES];
    uint32_t num_objs;
} waiter;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static waiter g_list = {.prev = &g_list, .next = &g_list};

static hos_event *g_event_pool;
static hos_event *g_event_free;

static void list_add(waiter *w) {
    w->prev = g_list.prev;
    w->next = &g_list;
    g_list.prev->next = w;
    g_list.prev = w;
}

static void list_remove(waiter *w) {
    w->prev->next = w->next;
    w->next->prev = w->prev;
    w->prev = w->next = NULL;
}

static void waiter_init(waiter *w, wait_kind kind, uint64_t key) {
    memset(w, 0, sizeof(*w));
    w->kind = kind;
    w->key = key;
    w->thread = hos_current_thread;
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&w->cond, &attr);
    pthread_condattr_destroy(&attr);
}

static void wake(waiter *w, hos_result result) {
    list_remove(w);
    w->woken = 1;
    w->result = result;
    pthread_cond_signal(&w->cond);
}

/* Block with the lock held. Returns with the lock held and the waiter off the list. */
static hos_result block(waiter *w, uint64_t timeout_ns) {
    list_add(w);
    if (timeout_ns == HOS_TIMEOUT_INFINITE) {
        while (!w->woken) {
            pthread_cond_wait(&w->cond, &g_lock);
        }
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t ns = (uint64_t)ts.tv_nsec + timeout_ns;
        ts.tv_sec += (time_t)(ns / 1000000000ull);
        ts.tv_nsec = (long)(ns % 1000000000ull);
        while (!w->woken) {
            int rc = pthread_cond_timedwait(&w->cond, &g_lock, &ts);
            if (rc == ETIMEDOUT) {
                break;
            }
        }
        if (!w->woken) {
            list_remove(w);
            w->result = HOS_KERNEL_TIMED_OUT;
        }
    }
    pthread_cond_destroy(&w->cond);
    return w->result;
}

static uint32_t load32(uint64_t addr) {
    return __atomic_load_n((uint32_t *)addr, __ATOMIC_SEQ_CST);
}

static void store32(uint64_t addr, uint32_t v) {
    __atomic_store_n((uint32_t *)addr, v, __ATOMIC_SEQ_CST);
}

static int32_t count_waiters(wait_kind kind, uint64_t key) {
    int32_t n = 0;
    for (waiter *w = g_list.next; w != &g_list; w = w->next) {
        if (w->kind == kind && w->key == key) {
            n++;
        }
    }
    return n;
}

static waiter *first_waiter(wait_kind kind, uint64_t key) {
    for (waiter *w = g_list.next; w != &g_list; w = w->next) {
        if (w->kind == kind && w->key == key) {
            return w;
        }
    }
    return NULL;
}

/* ---- mutex ----------------------------------------------------------------- */

/* Hand the mutex at `addr` to its next waiter, or clear it. Lock held. */
static void mutex_release(uint64_t addr) {
    waiter *w = first_waiter(WAIT_MUTEX, addr);
    if (!w) {
        store32(addr, 0);
        return;
    }
    list_remove(w);
    uint32_t v = w->thread->handle;
    if (first_waiter(WAIT_MUTEX, addr)) {
        v |= HOS_MUTEX_HAS_LISTENERS;
    }
    store32(addr, v);
    w->woken = 1;
    w->result = HOS_RESULT_SUCCESS;
    pthread_cond_signal(&w->cond);
}

/* svcArbitrateLock(w0 owner_handle, x1 mutex_addr, w2 self_handle) */
void svc_arbitrate_lock(hos_svc_regs *r) {
    uint32_t owner = (uint32_t)r->x[0];
    uint64_t addr = r->x[1];
    if (addr & 3) {
        r->x[0] = HOS_KERNEL_INVALID_ADDRESS;
        return;
    }
    pthread_mutex_lock(&g_lock);
    if (load32(addr) != (owner | HOS_MUTEX_HAS_LISTENERS)) {
        /* Owner changed before we got here; the caller retries its CAS. */
        pthread_mutex_unlock(&g_lock);
        r->x[0] = HOS_RESULT_SUCCESS;
        return;
    }
    waiter w;
    waiter_init(&w, WAIT_MUTEX, addr);
    r->x[0] = block(&w, HOS_TIMEOUT_INFINITE);
    pthread_mutex_unlock(&g_lock);
}

/* svcArbitrateUnlock(x0 mutex_addr) */
void svc_arbitrate_unlock(hos_svc_regs *r) {
    uint64_t addr = r->x[0];
    if (addr & 3) {
        r->x[0] = HOS_KERNEL_INVALID_ADDRESS;
        return;
    }
    pthread_mutex_lock(&g_lock);
    mutex_release(addr);
    pthread_mutex_unlock(&g_lock);
    r->x[0] = HOS_RESULT_SUCCESS;
}

/* ---- condition variables ---------------------------------------------------- */

/* svcWaitProcessWideKeyAtomic(x0 mutex_addr, x1 cv_addr, w2 self_handle, x3 timeout) */
void svc_wait_process_wide_key_atomic(hos_svc_regs *r) {
    uint64_t mutex_addr = r->x[0];
    uint64_t cv_addr = r->x[1];
    uint64_t timeout = r->x[3];
    if ((mutex_addr & 3) || (cv_addr & 3)) {
        r->x[0] = HOS_KERNEL_INVALID_ADDRESS;
        return;
    }
    pthread_mutex_lock(&g_lock);
    mutex_release(mutex_addr);
    waiter w;
    waiter_init(&w, WAIT_CONDVAR, cv_addr);
    w.mutex_addr = mutex_addr;
    hos_result res = block(&w, timeout);
    pthread_mutex_unlock(&g_lock);
    /* On timeout the caller re-locks the mutex itself, as on HOS. */
    r->x[0] = res;
}

/* Give the mutex to a condvar waiter, or park it on the mutex. Lock held. */
static void condvar_wake_one(waiter *w) {
    list_remove(w);
    uint32_t v = load32(w->mutex_addr);
    if (v == 0) {
        store32(w->mutex_addr, w->thread->handle);
        w->woken = 1;
        w->result = HOS_RESULT_SUCCESS;
        pthread_cond_signal(&w->cond);
        return;
    }
    store32(w->mutex_addr, v | HOS_MUTEX_HAS_LISTENERS);
    w->kind = WAIT_MUTEX;
    w->key = w->mutex_addr;
    list_add(w);
}

/* svcSignalProcessWideKey(x0 cv_addr, w1 count) */
void svc_signal_process_wide_key(hos_svc_regs *r) {
    uint64_t cv_addr = r->x[0];
    int32_t count = (int32_t)r->x[1];
    pthread_mutex_lock(&g_lock);
    int32_t woken = 0;
    waiter *w;
    while ((count < 0 || woken < count) && (w = first_waiter(WAIT_CONDVAR, cv_addr)) != NULL) {
        condvar_wake_one(w);
        woken++;
    }
    if (!first_waiter(WAIT_CONDVAR, cv_addr)) {
        store32(cv_addr, 0);
    }
    pthread_mutex_unlock(&g_lock);
    r->x[0] = HOS_RESULT_SUCCESS;
}

/* ---- address arbiter ---------------------------------------------------------- */

enum {
    ARB_WAIT_IF_LESS_THAN = 0,
    ARB_DECREMENT_AND_WAIT_IF_LESS_THAN = 1,
    ARB_WAIT_IF_EQUAL = 2,
};

enum {
    ARB_SIGNAL = 0,
    ARB_SIGNAL_AND_INCREMENT_IF_EQUAL = 1,
    ARB_SIGNAL_AND_MODIFY_BY_WAITING_COUNT_IF_EQUAL = 2,
};

/* svcWaitForAddress(x0 addr, w1 type, w2 value, x3 timeout) */
void svc_wait_for_address(hos_svc_regs *r) {
    uint64_t addr = r->x[0];
    uint32_t type = (uint32_t)r->x[1];
    int32_t value = (int32_t)r->x[2];
    uint64_t timeout = r->x[3];
    if (addr & 3) {
        r->x[0] = HOS_KERNEL_INVALID_ADDRESS;
        return;
    }
    pthread_mutex_lock(&g_lock);
    int32_t cur = (int32_t)load32(addr);
    int proceed;
    switch (type) {
    case ARB_WAIT_IF_LESS_THAN:
        proceed = cur < value;
        break;
    case ARB_DECREMENT_AND_WAIT_IF_LESS_THAN:
        proceed = cur < value;
        if (proceed) {
            store32(addr, (uint32_t)(cur - 1));
        }
        break;
    case ARB_WAIT_IF_EQUAL:
        proceed = cur == value;
        break;
    default:
        pthread_mutex_unlock(&g_lock);
        r->x[0] = HOS_KERNEL_INVALID_ENUM_VALUE;
        return;
    }
    if (!proceed) {
        pthread_mutex_unlock(&g_lock);
        r->x[0] = HOS_KERNEL_INVALID_STATE;
        return;
    }
    if (timeout == 0) {
        pthread_mutex_unlock(&g_lock);
        r->x[0] = HOS_KERNEL_TIMED_OUT;
        return;
    }
    waiter w;
    waiter_init(&w, WAIT_ADDRESS, addr);
    r->x[0] = block(&w, timeout);
    pthread_mutex_unlock(&g_lock);
}

/* svcSignalToAddress(x0 addr, w1 type, w2 value, w3 count) */
void svc_signal_to_address(hos_svc_regs *r) {
    uint64_t addr = r->x[0];
    uint32_t type = (uint32_t)r->x[1];
    int32_t value = (int32_t)r->x[2];
    int32_t count = (int32_t)r->x[3];
    if (addr & 3) {
        r->x[0] = HOS_KERNEL_INVALID_ADDRESS;
        return;
    }
    pthread_mutex_lock(&g_lock);
    int32_t waiting = count_waiters(WAIT_ADDRESS, addr);
    int32_t to_wake = (count < 0 || count > waiting) ? waiting : count;

    switch (type) {
    case ARB_SIGNAL:
        break;
    case ARB_SIGNAL_AND_INCREMENT_IF_EQUAL:
        if ((int32_t)load32(addr) != value) {
            pthread_mutex_unlock(&g_lock);
            r->x[0] = HOS_KERNEL_INVALID_STATE;
            return;
        }
        store32(addr, (uint32_t)(value + 1));
        break;
    case ARB_SIGNAL_AND_MODIFY_BY_WAITING_COUNT_IF_EQUAL:
        if ((int32_t)load32(addr) != value) {
            pthread_mutex_unlock(&g_lock);
            r->x[0] = HOS_KERNEL_INVALID_STATE;
            return;
        }
        store32(addr, (uint32_t)((waiting - to_wake > 0) ? value - 1 : value + 1));
        break;
    default:
        pthread_mutex_unlock(&g_lock);
        r->x[0] = HOS_KERNEL_INVALID_ENUM_VALUE;
        return;
    }
    for (int32_t i = 0; i < to_wake; i++) {
        waiter *w = first_waiter(WAIT_ADDRESS, addr);
        if (!w) {
            break;
        }
        wake(w, HOS_RESULT_SUCCESS);
    }
    pthread_mutex_unlock(&g_lock);
    r->x[0] = HOS_RESULT_SUCCESS;
}

/* ---- waitable objects: events and threads ------------------------------------- */

static int object_signaled(void *obj, hos_obj_type type) {
    switch (type) {
    case HOS_OBJ_READABLE_EVENT:
        return ((hos_event *)obj)->signaled;
    case HOS_OBJ_THREAD:
        return ((hos_thread *)obj)->exited;
    default:
        return 0;
    }
}

/* Wake every WAIT_SYNC waiter that includes `obj`. Lock held. */
static void signal_object(void *obj) {
    for (waiter *w = g_list.next; w != &g_list;) {
        waiter *next = w->next;
        if (w->kind == WAIT_SYNC) {
            for (uint32_t i = 0; i < w->num_objs; i++) {
                if (w->objs[i] == obj) {
                    w->index = (int32_t)i;
                    wake(w, HOS_RESULT_SUCCESS);
                    break;
                }
            }
        }
        w = next;
    }
}

void hos_sync_thread_exited(hos_thread *t) {
    pthread_mutex_lock(&g_lock);
    t->exited = 1;
    signal_object(t);
    pthread_mutex_unlock(&g_lock);
}

static hos_result event_pool_init(void) {
    g_event_pool = mmap(NULL, sizeof(hos_event) * EVENT_POOL, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_event_pool == MAP_FAILED) {
        g_event_pool = NULL;
        return HOS_KERNEL_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < EVENT_POOL; i++) {
        g_event_pool[i].next_free = (i + 1 < EVENT_POOL) ? &g_event_pool[i + 1] : NULL;
    }
    g_event_free = &g_event_pool[0];
    return HOS_RESULT_SUCCESS;
}

hos_result hos_event_create(uint32_t *out_writable, uint32_t *out_readable) {
    pthread_mutex_lock(&g_lock);
    if (!g_event_pool && HOS_FAILED(event_pool_init())) {
        pthread_mutex_unlock(&g_lock);
        return HOS_KERNEL_OUT_OF_MEMORY;
    }
    hos_event *e = g_event_free;
    if (!e) {
        pthread_mutex_unlock(&g_lock);
        return HOS_KERNEL_OUT_OF_RESOURCE;
    }
    g_event_free = e->next_free;
    e->signaled = 0;
    e->refs = 2;
    pthread_mutex_unlock(&g_lock);

    uint32_t w = hos_handle_alloc(HOS_OBJ_WRITABLE_EVENT, e);
    uint32_t rd = hos_handle_alloc(HOS_OBJ_READABLE_EVENT, e);
    if (!w || !rd) {
        if (w) {
            hos_handle_close(w);
        }
        if (rd) {
            hos_handle_close(rd);
        }
        pthread_mutex_lock(&g_lock);
        e->next_free = g_event_free;
        g_event_free = e;
        pthread_mutex_unlock(&g_lock);
        return HOS_KERNEL_OUT_OF_HANDLES;
    }
    *out_writable = w;
    *out_readable = rd;
    return HOS_RESULT_SUCCESS;
}

void hos_event_release(hos_event *e) {
    if (__atomic_sub_fetch(&e->refs, 1, __ATOMIC_SEQ_CST) != 0) {
        return;
    }
    pthread_mutex_lock(&g_lock);
    e->next_free = g_event_free;
    g_event_free = e;
    pthread_mutex_unlock(&g_lock);
}

void hos_event_signal(hos_event *e) {
    pthread_mutex_lock(&g_lock);
    e->signaled = 1;
    signal_object(e);
    pthread_mutex_unlock(&g_lock);
}

/* svcCreateEvent(out w1 writable, out w2 readable) */
void svc_create_event(hos_svc_regs *r) {
    uint32_t w = 0, rd = 0;
    r->x[0] = hos_event_create(&w, &rd);
    r->x[1] = w;
    r->x[2] = rd;
}

/* svcSignalEvent(w0 writable) */
void svc_signal_event(hos_svc_regs *r) {
    hos_event *e = hos_handle_get((uint32_t)r->x[0], HOS_OBJ_WRITABLE_EVENT);
    if (!e) {
        r->x[0] = HOS_KERNEL_INVALID_HANDLE;
        return;
    }
    hos_event_signal(e);
    r->x[0] = HOS_RESULT_SUCCESS;
}

/* svcClearEvent(w0 writable or readable) */
void svc_clear_event(hos_svc_regs *r) {
    hos_obj_type type;
    hos_event *e = hos_handle_lookup((uint32_t)r->x[0], &type);
    if (!e || (type != HOS_OBJ_WRITABLE_EVENT && type != HOS_OBJ_READABLE_EVENT)) {
        r->x[0] = HOS_KERNEL_INVALID_HANDLE;
        return;
    }
    pthread_mutex_lock(&g_lock);
    e->signaled = 0;
    pthread_mutex_unlock(&g_lock);
    r->x[0] = HOS_RESULT_SUCCESS;
}

/* svcResetSignal(w0 readable event): clears; InvalidState when it was not signaled. */
void svc_reset_signal(hos_svc_regs *r) {
    hos_event *e = hos_handle_get((uint32_t)r->x[0], HOS_OBJ_READABLE_EVENT);
    if (!e) {
        r->x[0] = HOS_KERNEL_INVALID_HANDLE;
        return;
    }
    pthread_mutex_lock(&g_lock);
    hos_result res = e->signaled ? HOS_RESULT_SUCCESS : HOS_KERNEL_INVALID_STATE;
    e->signaled = 0;
    pthread_mutex_unlock(&g_lock);
    r->x[0] = res;
}

/* svcWaitSynchronization(out w1 index, x1 handles*, w2 count, x3 timeout) */
void svc_wait_synchronization(hos_svc_regs *r) {
    const uint32_t *handles = (const uint32_t *)r->x[1];
    uint32_t count = (uint32_t)r->x[2];
    uint64_t timeout = r->x[3];
    if (count > HOS_WAIT_MAX_HANDLES) {
        r->x[0] = HOS_KERNEL_OUT_OF_RANGE;
        return;
    }
    if (count && !handles) {
        r->x[0] = HOS_KERNEL_INVALID_POINTER;
        return;
    }

    waiter w;
    waiter_init(&w, WAIT_SYNC, 0);
    for (uint32_t i = 0; i < count; i++) {
        hos_obj_type type;
        void *obj = hos_handle_lookup(handles[i], &type);
        if (handles[i] == HOS_HANDLE_CURRENT_THREAD) {
            obj = hos_current_thread;
            type = HOS_OBJ_THREAD;
        }
        if (!obj || (type != HOS_OBJ_READABLE_EVENT && type != HOS_OBJ_THREAD)) {
            HOS_LOGW(MOD, "WaitSynchronization: handle %#x is not waitable", handles[i]);
            r->x[0] = HOS_KERNEL_INVALID_HANDLE;
            return;
        }
        w.objs[i] = obj;
        w.types[i] = type;
    }
    w.num_objs = count;
    if (hos_log_get_level() >= HOS_LOG_TRACE) {
        HOS_LOGT(MOD, "WaitSynchronization on %u handle(s): %#x%s, timeout %#llx", count,
                 count ? handles[0] : 0, count > 1 ? " ..." : "", (unsigned long long)timeout);
    }

    pthread_mutex_lock(&g_lock);
    if (hos_current_thread && hos_current_thread->cancel_pending) {
        hos_current_thread->cancel_pending = 0;
        pthread_mutex_unlock(&g_lock);
        r->x[0] = HOS_KERNEL_CANCELLED;
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (object_signaled(w.objs[i], w.types[i])) {
            pthread_mutex_unlock(&g_lock);
            r->x[0] = HOS_RESULT_SUCCESS;
            r->x[1] = i;
            return;
        }
    }
    if (timeout == 0) {
        pthread_mutex_unlock(&g_lock);
        r->x[0] = HOS_KERNEL_TIMED_OUT;
        return;
    }
    hos_result res = block(&w, timeout);
    pthread_mutex_unlock(&g_lock);
    r->x[0] = res;
    r->x[1] = (res == HOS_RESULT_SUCCESS) ? (uint64_t)w.index : 0;
}

/* svcCancelSynchronization(w0 thread handle) */
void svc_cancel_synchronization(hos_svc_regs *r) {
    hos_thread *t = hos_thread_from_handle((uint32_t)r->x[0]);
    if (!t) {
        r->x[0] = HOS_KERNEL_INVALID_HANDLE;
        return;
    }
    pthread_mutex_lock(&g_lock);
    int found = 0;
    for (waiter *w = g_list.next; w != &g_list; w = w->next) {
        if (w->kind == WAIT_SYNC && w->thread == t) {
            wake(w, HOS_KERNEL_CANCELLED);
            found = 1;
            break;
        }
    }
    if (!found) {
        t->cancel_pending = 1;
    }
    pthread_mutex_unlock(&g_lock);
    r->x[0] = HOS_RESULT_SUCCESS;
}

/* ---- service-side helpers ------------------------------------------------------ */

uint32_t hos_ipc_event_create(void **ev) {
    uint32_t w = 0, rd = 0;
    if (HOS_FAILED(hos_event_create(&w, &rd))) {
        return 0;
    }
    hos_event *e = hos_handle_get(rd, HOS_OBJ_READABLE_EVENT);
    /* The service keeps the writable side forever; the game owns the readable handle. */
    *ev = e;
    return rd;
}

void hos_ipc_event_signal(void *ev) {
    hos_event_signal(ev);
}

void hos_ipc_event_clear(void *ev) {
    hos_event *e = ev;
    pthread_mutex_lock(&g_lock);
    e->signaled = 0;
    pthread_mutex_unlock(&g_lock);
}

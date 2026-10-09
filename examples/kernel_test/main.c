/*
 * kernel_test: exercises libhos_cpu and the service libraries the way an SDK
 * runtime would, from inside a real NSO with MOD0 and relocations. Every
 * check prints PASS or FAIL through svcOutputDebugString; the smoke test
 * greps for them. Needs HOS_SDMC (a directory) and HOS_ROMFS (a file) set.
 */

#include "hos.h"
#include "ipc_client.h"

#define CHECK(name, cond)                                                                          \
    do {                                                                                           \
        if (cond) {                                                                                \
            hos_puts("PASS " name);                                                                \
        } else {                                                                                   \
            hos_puts("FAIL " name);                                                                \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

#define ALIAS_REGION 0x2000000000ull

static int g_failures;
static uint32_t g_main_handle;
static uint8_t *g_heap; /* 2 MiB from SetHeapSize; IPC buffers live here */

/* A table of pointers: forces R_AARCH64_RELATIVE relocations into .data.rel.ro. */
static const char *const g_names[] = {"heap",    "event", "mutex",   "condvar",
                                      "arbiter", "ipc",   "services"};

typedef struct thread_ctx {
    uint32_t handle;
    uint32_t *mutex;
    uint32_t *cv;
    volatile uint32_t *counter;
    volatile int32_t *word;
    uint32_t event;
    uint8_t stack[0x4000] __attribute__((aligned(16)));
} thread_ctx;

static thread_ctx g_threads[3];

static uint32_t spawn(thread_ctx *t, void (*entry)(void *)) {
    uint32_t rc = svc_create_thread(&t->handle, entry, t, t->stack + sizeof(t->stack), 44, -2);
    if (rc) {
        return rc;
    }
    return svc_start_thread(t->handle);
}

static uint32_t wait_one(uint32_t handle, uint64_t timeout) {
    int32_t idx = -1;
    return svc_wait_synchronization(&idx, &handle, 1, timeout);
}

/* ---- threads ----------------------------------------------------------------- */

static void event_thread(void *arg) {
    thread_ctx *t = arg;
    svc_sleep_thread(1000000);
    *t->counter = 42;
    svc_signal_event(t->event);
    svc_exit_thread();
}

static void counter_thread(void *arg) {
    thread_ctx *t = arg;
    for (int i = 0; i < 2000; i++) {
        hos_mutex_lock(t->mutex, t->handle);
        uint32_t v = *t->counter;
        if ((i & 7) == 0) {
            svc_sleep_thread(0); /* yield while holding the lock to force contention */
        }
        *t->counter = v + 1;
        hos_mutex_unlock(t->mutex, t->handle);
    }
    svc_exit_thread();
}

static void condvar_thread(void *arg) {
    thread_ctx *t = arg;
    svc_sleep_thread(2000000);
    hos_mutex_lock(t->mutex, t->handle);
    *t->counter = 1;
    hos_mutex_unlock(t->mutex, t->handle);
    svc_signal_process_wide_key(t->cv, 1);
    svc_exit_thread();
}

static void arbiter_thread(void *arg) {
    thread_ctx *t = arg;
    svc_sleep_thread(2000000);
    __atomic_store_n(t->word, 1, __ATOMIC_SEQ_CST);
    svc_signal_to_address((void *)t->word, 0, 0, 1);
    svc_exit_thread();
}

/* ---- kernel tests ---------------------------------------------------------------- */

static void test_heap(void) {
    void *heap = 0;
    uint32_t rc = svc_set_heap_size(0x200000, &heap);
    CHECK("heap: SetHeapSize", rc == 0 && heap != 0);
    g_heap = heap;
    volatile uint64_t *p = heap;
    p[0] = 0x1234567890ABCDEFull;
    p[0x200000 / 8 - 1] = 7;
    CHECK("heap: writable", p[0] == 0x1234567890ABCDEFull && p[0x200000 / 8 - 1] == 7);
    uint64_t info = 0;
    rc = svc_get_info(&info, 4, 0xFFFF8001, 0);
    CHECK("heap: GetInfo matches", rc == 0 && (void *)info == heap);
}

static void test_event_and_thread_exit(void) {
    uint32_t w = 0, r = 0;
    uint32_t rc = svc_create_event(&w, &r);
    CHECK("event: CreateEvent", rc == 0 && w && r);

    volatile uint32_t counter = 0;
    thread_ctx *t = &g_threads[0];
    t->counter = &counter;
    t->event = w;
    CHECK("event: spawn", spawn(t, event_thread) == 0);

    rc = wait_one(r, 2000000000ull);
    CHECK("event: WaitSynchronization", rc == 0 && counter == 42);
    rc = wait_one(r, 0);
    CHECK("event: stays signaled", rc == 0);
    svc_clear_event(r);
    rc = wait_one(r, 0);
    CHECK("event: cleared -> TimedOut", rc == 0xEA01);

    rc = wait_one(t->handle, 2000000000ull);
    CHECK("thread: exit is waitable", rc == 0);
    CHECK("thread: CloseHandle", svc_close_handle(t->handle) == 0);
    CHECK("event: CloseHandle", svc_close_handle(w) == 0 && svc_close_handle(r) == 0);
}

static void test_mutex(void) {
    static uint32_t mutex;
    volatile uint32_t counter = 0;
    thread_ctx *a = &g_threads[0];
    thread_ctx *b = &g_threads[1];
    a->mutex = b->mutex = &mutex;
    a->counter = b->counter = &counter;
    CHECK("mutex: spawn", spawn(a, counter_thread) == 0 && spawn(b, counter_thread) == 0);

    uint32_t handles[2] = {a->handle, b->handle};
    for (int i = 0; i < 2; i++) {
        int32_t idx = -1;
        uint32_t rc = svc_wait_synchronization(&idx, &handles[i], 1, 10000000000ull);
        if (rc != 0) {
            hos_puts("FAIL mutex: thread did not exit");
            g_failures++;
            return;
        }
    }
    CHECK("mutex: counter == 4000", counter == 4000);
    CHECK("mutex: released", mutex == 0);
    svc_close_handle(a->handle);
    svc_close_handle(b->handle);
}

static void test_condvar(void) {
    static uint32_t mutex, cv;
    volatile uint32_t ready = 0;
    thread_ctx *t = &g_threads[0];
    t->mutex = &mutex;
    t->cv = &cv;
    t->counter = &ready;
    CHECK("condvar: spawn", spawn(t, condvar_thread) == 0);

    hos_mutex_lock(&mutex, g_main_handle);
    uint32_t rc = 0;
    int rounds = 0;
    while (!ready && rounds++ < 100) {
        cv = 1;
        rc = svc_wait_process_wide_key_atomic(&mutex, &cv, g_main_handle, 5000000000ull);
        if (rc != 0) {
            break;
        }
    }
    CHECK("condvar: woke with mutex held",
          rc == 0 && ready == 1 && (mutex & ~HOS_MUTEX_HAS_LISTENERS) == g_main_handle);
    hos_mutex_unlock(&mutex, g_main_handle);

    hos_mutex_lock(&mutex, g_main_handle);
    cv = 1;
    rc = svc_wait_process_wide_key_atomic(&mutex, &cv, g_main_handle, 1000000);
    CHECK("condvar: timeout", rc == 0xEA01);
    /* On timeout the mutex is not held; take it again and release it. */
    hos_mutex_lock(&mutex, g_main_handle);
    hos_mutex_unlock(&mutex, g_main_handle);
    CHECK("condvar: mutex clean after timeout", mutex == 0);

    wait_one(t->handle, 2000000000ull);
    svc_close_handle(t->handle);
}

static void test_arbiter(void) {
    static volatile int32_t word;
    word = 0;
    thread_ctx *t = &g_threads[0];
    t->word = &word;
    CHECK("arbiter: spawn", spawn(t, arbiter_thread) == 0);

    uint32_t rc = svc_wait_for_address((void *)&word, 2, 0, 5000000000ull); /* WaitIfEqual */
    CHECK("arbiter: WaitIfEqual woken", rc == 0 && word == 1);
    rc = svc_wait_for_address((void *)&word, 2, 0, 0);
    CHECK("arbiter: condition false -> InvalidState", rc == 0xFA01);
    rc = svc_signal_to_address((void *)&word, 1, 1, 1); /* SignalAndIncrementIfEqual */
    CHECK("arbiter: increment", rc == 0 && word == 2);

    wait_one(t->handle, 2000000000ull);
    svc_close_handle(t->handle);
}

/* ---- IPC helpers ------------------------------------------------------------------ */

static uint32_t *tls_ipc(void) {
    uint64_t tls;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tls));
    return (uint32_t *)tls;
}

static uint32_t ipc_call(uint32_t session, const ipc_request *req, ipc_response *resp) {
    uint32_t *tls = tls_ipc();
    ipc_write_request(tls, req);
    uint32_t rc = svc_send_sync_request(session);
    if (rc) {
        return rc;
    }
    if (ipc_parse_response(tls, resp, req->domain, req->type >= 16) != 0) {
        return 0xFFFFFFFFu;
    }
    return resp->result;
}

static uint64_t service_name(const char *s) {
    uint64_t v = 0;
    for (int i = 0; i < 8 && s[i]; i++) {
        v |= (uint64_t)(uint8_t)s[i] << (8 * i);
    }
    return v;
}

static uint32_t get_service(uint32_t sm, const char *name) {
    uint64_t n = service_name(name);
    ipc_request req = {.type = IPC_TYPE_REQUEST, .cmd = 1, .data = &n, .data_size = 8};
    ipc_response resp;
    if (ipc_call(sm, &req, &resp) != 0 || resp.num_move != 1) {
        return 0;
    }
    return resp.move_handles[0];
}

/* Simple command: in-args only, returns the result and leaves resp filled. */
static uint32_t cmd(uint32_t session, uint32_t id, const void *in, uint32_t in_size,
                    ipc_response *resp) {
    ipc_request req = {.type = IPC_TYPE_REQUEST, .cmd = id, .data = in, .data_size = in_size};
    return ipc_call(session, &req, resp);
}

static uint32_t cmd_pid(uint32_t session, uint32_t id, const void *in, uint32_t in_size,
                        ipc_response *resp) {
    ipc_request req = {
        .type = IPC_TYPE_REQUEST, .cmd = id, .data = in, .data_size = in_size, .send_pid = 1};
    return ipc_call(session, &req, resp);
}

/* nn::diag log packet: header + one TextLog field. */
static size_t make_log_packet(uint8_t *out, const char *text) {
    size_t n = hos_strlen(text);
    ipc_memset(out, 0, 0x18);
    out[0x10] = 3; /* flags: head | tail */
    out[0x12] = 1; /* severity Info */
    uint32_t payload = (uint32_t)(2 + n);
    ipc_memcpy(out + 0x14, &payload, 4);
    out[0x18] = 2;          /* TextLog */
    out[0x19] = (uint8_t)n; /* uleb128, n < 128 */
    ipc_memcpy(out + 0x1A, text, n);
    return 0x1A + n;
}

static void test_ipc(void) {
    uint32_t sm = 0;
    uint32_t rc = svc_connect_to_named_port(&sm, "sm:");
    CHECK("ipc: ConnectToNamedPort(sm:)", rc == 0 && sm != 0);

    ipc_response resp;
    ipc_request req = {.type = IPC_TYPE_REQUEST, .cmd = 0, .send_pid = 1};
    rc = ipc_call(sm, &req, &resp);
    CHECK("ipc: sm RegisterClient", rc == 0);

    uint32_t lm = get_service(sm, "lm");
    CHECK("ipc: sm GetService(lm)", lm != 0);

    uint64_t name = service_name("nope");
    req = (ipc_request){.type = IPC_TYPE_REQUEST, .cmd = 1, .data = &name, .data_size = 8};
    rc = ipc_call(sm, &req, &resp);
    CHECK("ipc: sm GetService(nope) fails", rc == 0x1015);

    /* lm as a plain session: OpenLogger returns a move handle. */
    rc = cmd_pid(lm, 0, NULL, 0, &resp);
    uint32_t logger = resp.num_move ? resp.move_handles[0] : 0;
    CHECK("ipc: lm OpenLogger", rc == 0 && logger != 0);

    static uint8_t packet[256];
    size_t len = make_log_packet(packet, "hello via lm (session)");
    req = (ipc_request){.type = IPC_TYPE_REQUEST, .cmd = 0, .a_buf = packet, .a_size = len};
    rc = ipc_call(logger, &req, &resp);
    CHECK("ipc: ILogger Log", rc == 0);
    CHECK("ipc: close logger", svc_close_handle(logger) == 0);

    /* lm as a domain: OpenLogger returns an object id. */
    req = (ipc_request){.type = IPC_TYPE_CONTROL, .cmd = 0};
    rc = ipc_call(lm, &req, &resp);
    uint32_t root_id = resp.data_words ? resp.data[0] : 0;
    CHECK("ipc: ConvertToDomain", rc == 0 && root_id == 1);

    req = (ipc_request){.type = IPC_TYPE_REQUEST,
                        .cmd = 0,
                        .send_pid = 1,
                        .domain = 1,
                        .domain_cmd = 1,
                        .object_id = root_id};
    rc = ipc_call(lm, &req, &resp);
    uint32_t logger_id = resp.num_objects ? resp.object_ids[0] : 0;
    CHECK("ipc: domain OpenLogger", rc == 0 && logger_id == 2);

    len = make_log_packet(packet, "hello via lm (domain)");
    req = (ipc_request){.type = IPC_TYPE_REQUEST,
                        .cmd = 0,
                        .a_buf = packet,
                        .a_size = len,
                        .domain = 1,
                        .domain_cmd = 1,
                        .object_id = logger_id};
    rc = ipc_call(lm, &req, &resp);
    CHECK("ipc: domain ILogger Log", rc == 0);

    req = (ipc_request){
        .type = IPC_TYPE_REQUEST, .domain = 1, .domain_cmd = 2, .object_id = logger_id};
    rc = ipc_call(lm, &req, &resp);
    CHECK("ipc: domain close object", rc == 0);

    /* TIPC to sm, as firmware 12+ clients do. */
    name = service_name("lm");
    req = (ipc_request){.type = IPC_TYPE_TIPC(1), .data = &name, .data_size = 8};
    rc = ipc_call(sm, &req, &resp);
    CHECK("ipc: tipc GetService(lm)", rc == 0 && resp.num_move == 1);
    svc_close_handle(resp.move_handles[0]);

    CHECK("ipc: close sessions", svc_close_handle(lm) == 0 && svc_close_handle(sm) == 0);
    CHECK("ipc: closed handle rejected", svc_send_sync_request(lm) == 0xE401);
}

/* ---- services ------------------------------------------------------------------------ */

static void test_services(void) {
    uint32_t sm = 0;
    svc_connect_to_named_port(&sm, "sm:");
    ipc_response resp;
    uint32_t rc;
    uint8_t *buf = g_heap + 0x100000; /* IPC buffers must live in the 39-bit space */

    /* set:sys GetFirmwareVersion through a C (pointer out) buffer */
    uint32_t setsys = get_service(sm, "set:sys");
    CHECK("set:sys: GetService", setsys != 0);
    ipc_memset(buf, 0, 0x100);
    ipc_request req = {.type = IPC_TYPE_REQUEST, .cmd = 3, .c_buf = buf, .c_size = 0x100};
    rc = ipc_call(setsys, &req, &resp);
    CHECK("set:sys: GetFirmwareVersion", rc == 0 && buf[0] == 11 && buf[8] == 'N');
    svc_close_handle(setsys);

    /* time: wall clock and shared memory */
    uint32_t time = get_service(sm, "time:u");
    CHECK("time: GetService", time != 0);
    rc = cmd(time, 0, NULL, 0, &resp); /* GetStandardUserSystemClock */
    uint32_t clock = resp.num_move ? resp.move_handles[0] : 0;
    CHECK("time: GetStandardUserSystemClock", rc == 0 && clock != 0);
    rc = cmd(clock, 0, NULL, 0, &resp); /* GetCurrentTime */
    int64_t posix = 0;
    if (resp.data_words >= 2) {
        ipc_memcpy(&posix, resp.data, 8);
    }
    CHECK("time: GetCurrentTime is after 2024", rc == 0 && posix > 1704067200);
    svc_close_handle(clock);
    rc = cmd(time, 20, NULL, 0, &resp); /* GetSharedMemoryNativeHandle */
    uint32_t tshm = resp.num_copy ? resp.copy_handles[0] : 0;
    CHECK("time: GetSharedMemoryNativeHandle", rc == 0 && tshm != 0);
    uint64_t tmap = ALIAS_REGION + 0x100000;
    {
        register uint64_t x0 __asm__("x0") = tshm;
        register uint64_t x1 __asm__("x1") = tmap;
        register uint64_t x2 __asm__("x2") = 0x1000;
        register uint64_t x3 __asm__("x3") = 1;
        __asm__ volatile("svc #0x13" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : SVC_CLOBBERS);
        rc = (uint32_t)x0;
    }
    CHECK("time: MapSharedMemory", rc == 0 && *(volatile uint32_t *)tmap == 0);
    svc_close_handle(time);

    /* hid: applet resource shared memory */
    uint32_t hid = get_service(sm, "hid");
    CHECK("hid: GetService", hid != 0);
    uint64_t aruid = 1;
    rc = cmd_pid(hid, 0, &aruid, 8, &resp); /* CreateAppletResource */
    uint32_t ares = resp.num_move ? resp.move_handles[0] : 0;
    CHECK("hid: CreateAppletResource", rc == 0 && ares != 0);
    rc = cmd(ares, 0, NULL, 0, &resp); /* GetSharedMemoryHandle */
    uint32_t hshm = resp.num_copy ? resp.copy_handles[0] : 0;
    CHECK("hid: GetSharedMemoryHandle", rc == 0 && hshm != 0);
    uint64_t hmap = ALIAS_REGION + 0x200000;
    {
        register uint64_t x0 __asm__("x0") = hshm;
        register uint64_t x1 __asm__("x1") = hmap;
        register uint64_t x2 __asm__("x2") = 0x40000;
        register uint64_t x3 __asm__("x3") = 1;
        __asm__ volatile("svc #0x13" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : SVC_CLOBBERS);
        rc = (uint32_t)x0;
    }
    CHECK("hid: MapSharedMemory 0x40000", rc == 0 && *(volatile uint64_t *)(hmap + 0x3FFF8) == 0);
    struct {
        uint32_t style_set;
        uint32_t pad;
        uint64_t aruid;
    } style = {0x3F, 0, 1};
    rc = cmd_pid(hid, 100, &style, sizeof(style), &resp); /* SetSupportedNpadStyleSet */
    CHECK("hid: SetSupportedNpadStyleSet", rc == 0);
    svc_close_handle(ares);
    svc_close_handle(hid);

    /* fsp-srv: sdmc as a host directory, romfs as a storage */
    uint32_t fs = get_service(sm, "fsp-srv");
    CHECK("fs: GetService", fs != 0);
    uint64_t zero = 0;
    rc = cmd_pid(fs, 1, &zero, 8, &resp); /* SetCurrentProcess */
    CHECK("fs: SetCurrentProcess", rc == 0);
    rc = cmd(fs, 18, NULL, 0, &resp); /* OpenSdCardFileSystem */
    uint32_t sdmc = resp.num_move ? resp.move_handles[0] : 0;
    CHECK("fs: OpenSdCardFileSystem", rc == 0 && sdmc != 0);

    static const char k_path[0x301] = "/kernel_test.txt";
    struct {
        uint32_t option;
        uint32_t pad;
        int64_t size;
    } create = {0, 0, 0};
    req = (ipc_request){.type = IPC_TYPE_REQUEST,
                        .cmd = 1,
                        .x_buf = k_path,
                        .x_size = sizeof(k_path)}; /* DeleteFile, ignore result */
    ipc_call(sdmc, &req, &resp);
    req = (ipc_request){.type = IPC_TYPE_REQUEST,
                        .cmd = 0,
                        .data = &create,
                        .data_size = sizeof(create),
                        .x_buf = k_path,
                        .x_size = sizeof(k_path)};
    rc = ipc_call(sdmc, &req, &resp);
    CHECK("fs: CreateFile", rc == 0);
    rc = ipc_call(sdmc, &req, &resp);
    CHECK("fs: CreateFile twice -> PathAlreadyExists", rc == 0x402);

    uint32_t mode = 3; /* read | write */
    req = (ipc_request){.type = IPC_TYPE_REQUEST,
                        .cmd = 8,
                        .data = &mode,
                        .data_size = 4,
                        .x_buf = k_path,
                        .x_size = sizeof(k_path)};
    rc = ipc_call(sdmc, &req, &resp);
    uint32_t file = resp.num_move ? resp.move_handles[0] : 0;
    CHECK("fs: OpenFile", rc == 0 && file != 0);

    static const char k_text[] = "hello files";
    ipc_memcpy(buf, k_text, sizeof(k_text));
    struct {
        uint32_t option;
        uint32_t pad;
        int64_t offset;
        int64_t size;
    } io = {1, 0, 0, (int64_t)sizeof(k_text)};
    req = (ipc_request){.type = IPC_TYPE_REQUEST,
                        .cmd = 1,
                        .data = &io,
                        .data_size = sizeof(io),
                        .a_buf = buf,
                        .a_size = sizeof(k_text)};
    rc = ipc_call(file, &req, &resp);
    CHECK("fs: IFile Write", rc == 0);
    rc = cmd(file, 4, NULL, 0, &resp); /* GetSize */
    int64_t size = 0;
    if (resp.data_words >= 2) {
        ipc_memcpy(&size, resp.data, 8);
    }
    CHECK("fs: IFile GetSize", rc == 0 && size == (int64_t)sizeof(k_text));
    ipc_memset(buf + 0x1000, 0, 64);
    io.option = 0;
    req = (ipc_request){.type = IPC_TYPE_REQUEST,
                        .cmd = 0,
                        .data = &io,
                        .data_size = sizeof(io),
                        .b_buf = buf + 0x1000,
                        .b_size = 64};
    rc = ipc_call(file, &req, &resp);
    int64_t got = 0;
    if (resp.data_words >= 2) {
        ipc_memcpy(&got, resp.data, 8);
    }
    int same = 1;
    for (size_t i = 0; i < sizeof(k_text); i++) {
        same = same && buf[0x1000 + i] == (uint8_t)k_text[i];
    }
    CHECK("fs: IFile Read", rc == 0 && got == (int64_t)sizeof(k_text) && same);
    svc_close_handle(file);

    req = (ipc_request){
        .type = IPC_TYPE_REQUEST, .cmd = 7, .x_buf = k_path, .x_size = sizeof(k_path)};
    rc = ipc_call(sdmc, &req, &resp);
    CHECK("fs: GetEntryType file", rc == 0 && resp.data_words >= 1 && resp.data[0] == 1);
    req.cmd = 1; /* DeleteFile */
    rc = ipc_call(sdmc, &req, &resp);
    CHECK("fs: DeleteFile", rc == 0);
    req.cmd = 7;
    rc = ipc_call(sdmc, &req, &resp);
    CHECK("fs: GetEntryType gone -> PathNotFound", rc == 0x202);
    svc_close_handle(sdmc);

    rc = cmd(fs, 200, NULL, 0, &resp); /* OpenDataStorageByCurrentProcess */
    uint32_t storage = resp.num_move ? resp.move_handles[0] : 0;
    CHECK("fs: OpenDataStorageByCurrentProcess", rc == 0 && storage != 0);
    rc = cmd(storage, 4, NULL, 0, &resp); /* GetSize */
    size = 0;
    if (resp.data_words >= 2) {
        ipc_memcpy(&size, resp.data, 8);
    }
    CHECK("fs: IStorage GetSize", rc == 0 && size > 0);
    int64_t sr[2] = {0, 16};
    ipc_memset(buf + 0x2000, 0, 16);
    req = (ipc_request){.type = IPC_TYPE_REQUEST,
                        .cmd = 0,
                        .data = sr,
                        .data_size = 16,
                        .b_buf = buf + 0x2000,
                        .b_size = 16};
    rc = ipc_call(storage, &req, &resp);
    CHECK("fs: IStorage Read",
          rc == 0 && buf[0x2000] == 'R' && buf[0x2001] == 'O' && buf[0x2002] == 'M');
    svc_close_handle(storage);
    svc_close_handle(fs);

    /* applet: a quick spin through the proxy, as libnx appletInitialize does */
    uint32_t applet = get_service(sm, "appletOE");
    CHECK("applet: GetService", applet != 0);
    req = (ipc_request){.type = IPC_TYPE_REQUEST,
                        .cmd = 0,
                        .data = &zero,
                        .data_size = 8,
                        .send_pid = 1,
                        .copy_handles = {0xFFFF8001},
                        .num_copy = 1};
    rc = ipc_call(applet, &req, &resp);
    uint32_t proxy = resp.num_move ? resp.move_handles[0] : 0;
    CHECK("applet: OpenApplicationProxy", rc == 0 && proxy != 0);
    rc = cmd(proxy, 0, NULL, 0, &resp); /* GetCommonStateGetter */
    uint32_t csg = resp.num_move ? resp.move_handles[0] : 0;
    CHECK("applet: GetCommonStateGetter", rc == 0 && csg != 0);
    rc = cmd(csg, 9, NULL, 0, &resp); /* GetCurrentFocusState */
    CHECK("applet: in focus", rc == 0 && resp.data_words >= 1 && (resp.data[0] & 0xFF) == 1);
    rc = cmd(csg, 1, NULL, 0, &resp); /* ReceiveMessage */
    CHECK("applet: first message is FocusStateChanged",
          rc == 0 && resp.data_words >= 1 && resp.data[0] == 15);
    rc = cmd(csg, 1, NULL, 0, &resp);
    CHECK("applet: then NoMessage", rc == 0x680);
    svc_close_handle(csg);
    svc_close_handle(proxy);
    svc_close_handle(applet);
    svc_close_handle(sm);
}

void hos_main(uint32_t main_handle) {
    g_main_handle = main_handle;
    hos_puts("kernel_test start");
    char line[64];
    char *p = hos_str(line, "main thread handle ");
    hos_hex(p, main_handle);
    hos_puts(line);

    test_heap();
    test_event_and_thread_exit();
    test_mutex();
    test_condvar();
    test_arbiter();
    test_ipc();
    test_services();

    p = hos_str(line, "sections: ");
    for (int i = 0; i < 7; i++) {
        p = hos_str(p, g_names[i]);
        *p++ = ' ';
    }
    *p = '\0';
    hos_puts(line);

    if (g_failures) {
        p = hos_str(line, "kernel_test: failures ");
        hos_hex(p, (uint64_t)g_failures);
        hos_puts(line);
        svc_break(0, 0, 0);
    }
    hos_puts("kernel_test done");
    svc_exit_process();
}

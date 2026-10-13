/*
 * hbabi_test: what libnx's crt0 does with the homebrew ABI, from a freestanding
 * module. Walks the config entries in x0 and reports them, reads argv[0] back
 * through fsp-srv's SD card (the file hosrun serves at that path, from the
 * card or through HOS_SDMC_ALIAS), checks it is an NRO, and when its argv says
 * `next=<path>` writes that path into the next-load buffers, as nx-hbmenu does
 * before leaving; `loop` repeats the same argv so the chain never ends on its
 * own. Returns to the loader through lr. Every line starts with "hbabi_test:";
 * a failed check starts with "FAIL".
 */

#include "../kernel_test/hos.h"
#include "../kernel_test/ipc_client.h"

enum {
    ENTRY_END_OF_LIST = 0,
    ENTRY_MAIN_THREAD_HANDLE = 1,
    ENTRY_NEXT_LOAD_PATH = 2,
    ENTRY_ARGV = 5,
    ENTRY_APPLET_TYPE = 7,
    ENTRY_HOS_VERSION = 16,
};
#define ENTRY_FLAG_MANDATORY 1
#define NEXT_PATH_MAX        0x200
#define NEXT_ARGV_MAX        0x800

typedef struct config_entry {
    uint32_t key;
    uint32_t flags;
    uint64_t value[2];
} config_entry;

static char g_path[0x301];
static uint8_t g_head[0x40] __attribute__((aligned(0x1000)));

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

static uint32_t get_service(uint32_t sm, const char *name) {
    uint64_t n = 0;
    for (int i = 0; i < 8 && name[i]; i++) {
        n |= (uint64_t)(uint8_t)name[i] << (8 * i);
    }
    ipc_request req = {.type = IPC_TYPE_REQUEST, .cmd = 1, .data = &n, .data_size = 8};
    ipc_response resp;
    if (ipc_call(sm, &req, &resp) != 0 || resp.num_move != 1) {
        return 0;
    }
    return resp.move_handles[0];
}

static int starts_with(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s++ != *prefix++) {
            return 0;
        }
    }
    return 1;
}

/* Open argv[0] on the SD card and read its first 0x40 bytes: "NRO0" at 0x10. */
static int read_self(const char *argv0, size_t len) {
    if (len < 6 || len - 5 >= sizeof(g_path) || hos_strlen("sdmc:") != 5 || argv0[0] != 's' ||
        argv0[4] != ':' || argv0[5] != '/') {
        hos_puts("FAIL hbabi_test: argv[0] is not an sdmc:/ path");
        return 0;
    }
    ipc_memcpy(g_path, argv0 + 5, len - 5);
    g_path[len - 5] = '\0';

    uint32_t sm = 0;
    ipc_response resp;
    if (svc_connect_to_named_port(&sm, "sm:") != 0) {
        hos_puts("FAIL hbabi_test: sm:");
        return 0;
    }
    ipc_request req = {.type = IPC_TYPE_REQUEST, .cmd = 0, .send_pid = 1};
    ipc_call(sm, &req, &resp); /* RegisterClient */
    uint32_t fs = get_service(sm, "fsp-srv");
    uint64_t zero = 0;
    req = (ipc_request){
        .type = IPC_TYPE_REQUEST, .cmd = 1, .data = &zero, .data_size = 8, .send_pid = 1};
    ipc_call(fs, &req, &resp); /* SetCurrentProcess */
    req = (ipc_request){.type = IPC_TYPE_REQUEST, .cmd = 18};
    uint32_t rc = ipc_call(fs, &req, &resp); /* OpenSdCardFileSystem */
    uint32_t sdmc = resp.num_move ? resp.move_handles[0] : 0;
    if (rc != 0 || !sdmc) {
        hos_puts("FAIL hbabi_test: OpenSdCardFileSystem");
        return 0;
    }
    uint32_t mode = 1; /* read */
    req = (ipc_request){.type = IPC_TYPE_REQUEST,
                        .cmd = 8,
                        .data = &mode,
                        .data_size = 4,
                        .x_buf = g_path,
                        .x_size = sizeof(g_path)};
    rc = ipc_call(sdmc, &req, &resp); /* OpenFile */
    uint32_t file = resp.num_move ? resp.move_handles[0] : 0;
    if (rc != 0 || !file) {
        hos_puts("FAIL hbabi_test: OpenFile(argv[0])");
        return 0;
    }
    struct {
        uint32_t option;
        uint32_t pad;
        int64_t offset;
        int64_t size;
    } io = {0, 0, 0, (int64_t)sizeof(g_head)};
    req = (ipc_request){.type = IPC_TYPE_REQUEST,
                        .cmd = 0,
                        .data = &io,
                        .data_size = sizeof(io),
                        .b_buf = g_head,
                        .b_size = sizeof(g_head)};
    rc = ipc_call(file, &req, &resp); /* Read */
    int64_t got = 0;
    if (resp.data_words >= 2) {
        ipc_memcpy(&got, resp.data, 8);
    }
    svc_close_handle(file);
    svc_close_handle(sdmc);
    svc_close_handle(fs);
    svc_close_handle(sm);
    if (rc != 0 || got != (int64_t)sizeof(g_head) || g_head[0x10] != 'N' || g_head[0x11] != 'R' ||
        g_head[0x12] != 'O' || g_head[0x13] != '0') {
        hos_puts("FAIL hbabi_test: argv[0] does not read back as an NRO");
        return 0;
    }
    hos_puts("hbabi_test: argv[0] is an NRO");
    return 1;
}

void hbabi_main(const config_entry *entries, uint64_t x1) {
    if (x1 != UINT64_MAX || !entries) {
        hos_puts("hbabi_test: NSO entry (x1 is the main thread handle, no config entries)");
        svc_exit_process();
    }
    uint32_t main_thread = 0, version = 0;
    const char *argv = NULL;
    char *next_path = NULL, *next_argv = NULL;
    int count = 0, mandatory_ok = 0;
    for (const config_entry *e = entries; e->key != ENTRY_END_OF_LIST; e++) {
        count++;
        switch (e->key) {
        case ENTRY_MAIN_THREAD_HANDLE:
            main_thread = (uint32_t)e->value[0];
            mandatory_ok = (e->flags & ENTRY_FLAG_MANDATORY) != 0;
            break;
        case ENTRY_NEXT_LOAD_PATH:
            next_path = (char *)e->value[0];
            next_argv = (char *)e->value[1];
            break;
        case ENTRY_ARGV:
            argv = (const char *)e->value[1];
            break;
        case ENTRY_HOS_VERSION:
            version = (uint32_t)e->value[0];
            break;
        default:
            break;
        }
    }
    char line[NEXT_ARGV_MAX + 64];
    char *p = hos_str(line, "hbabi_test: ");
    p = hos_hex(p, (uint64_t)count);
    p = hos_str(p, " entries, main thread handle ");
    p = hos_hex(p, main_thread);
    p = hos_str(p, ", HOS version ");
    hos_hex(p, version);
    hos_puts(line);
    if (!mandatory_ok || !main_thread) {
        hos_puts("FAIL hbabi_test: no mandatory main thread handle entry");
    }
    if (!next_path || !next_argv) {
        hos_puts("FAIL hbabi_test: no next load path entry");
    }
    if (!argv) {
        hos_puts("FAIL hbabi_test: no argv entry");
        svc_exit_process();
    }
    p = hos_str(line, "hbabi_test: argv \"");
    p = hos_str(p, argv);
    hos_str(p, "\"");
    hos_puts(line);

    /* argv[0], then the options this test understands. */
    size_t argv0_len = 0;
    while (argv[argv0_len] && argv[argv0_len] != ' ') {
        argv0_len++;
    }
    read_self(argv, argv0_len);

    const char *next = NULL;
    size_t next_len = 0;
    int loop = 0;
    for (const char *t = argv + argv0_len; *t;) {
        while (*t == ' ') {
            t++;
        }
        size_t n = 0;
        while (t[n] && t[n] != ' ') {
            n++;
        }
        if (starts_with(t, "next=")) {
            next = t + 5;
            next_len = n - 5;
        } else if (n == 4 && starts_with(t, "loop")) {
            loop = 1;
        }
        t += n;
    }
    if (next && next_path && next_argv && next_len < NEXT_PATH_MAX) {
        ipc_memcpy(next_path, next, next_len);
        next_path[next_len] = '\0';
        if (loop) {
            size_t n = hos_strlen(argv);
            ipc_memcpy(next_argv, argv, n < NEXT_ARGV_MAX ? n : NEXT_ARGV_MAX - 1);
            next_argv[n < NEXT_ARGV_MAX ? n : NEXT_ARGV_MAX - 1] = '\0';
        } else {
            ipc_memcpy(next_argv, next, next_len);
            hos_str(next_argv + next_len, " chained");
        }
        p = hos_str(line, "hbabi_test: next load ");
        hos_str(p, next_path);
        hos_puts(line);
    }
    hos_puts("hbabi_test: done, returning through lr");
}

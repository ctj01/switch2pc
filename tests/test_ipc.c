/*
 * IPC transport through the dispatcher: sm, lm, domains, TIPC. Uses the same
 * header-only client the kernel_test module uses. ARM64 only.
 */

#include <stdlib.h>
#include <string.h>

#include "common/hos_log.h"
#include "hos_cpu.h"
#include "ipc_client.h"
#include "test.h"

static int g_inited;

static void init_once(void) {
    if (g_inited) {
        return;
    }
    g_inited = 1;
    hos_log_set_level(HOS_LOG_WARN);
    static uint8_t fake_image[0x2000] __attribute__((aligned(0x1000)));
    hos_cpu_params p = {
        .num_modules = 1,
        .modules = {{.image_base = (uint64_t)fake_image,
                     .image_size = sizeof(fake_image),
                     .text_size = 0x1000,
                     .name = "test_ipc"}},
    };
    if (HOS_FAILED(hos_cpu_init(&p)) || !hos_cpu_adopt_current_thread()) {
        fprintf(stderr, "init failed\n");
        exit(1);
    }
}

static hos_svc_regs call(uint32_t id, uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3) {
    hos_svc_regs r = {{x0, x1, x2, x3, 0, 0, 0, 0}};
    hos_svc_dispatch(id, &r);
    return r;
}

static uint32_t connect_sm(void) {
    hos_svc_regs r = call(0x1F, 0, (uint64_t) "sm:", 0, 0);
    return r.x[0] == 0 ? (uint32_t)r.x[1] : 0;
}

static uint32_t ipc_call(uint32_t session, const ipc_request *req, ipc_response *resp) {
    uint32_t *tls = hos_cpu_current_tls();
    ipc_write_request(tls, req);
    hos_svc_regs r = call(0x21, session, 0, 0, 0);
    if (r.x[0] != 0) {
        return (uint32_t)r.x[0];
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

TEST(ipc_connect_named_port) {
    init_once();
    uint32_t sm = connect_sm();
    ASSERT_TRUE(sm != 0);
    hos_svc_regs r = call(0x1F, 0, (uint64_t) "nope", 0, 0);
    ASSERT_EQ(r.x[0], HOS_KERNEL_NOT_FOUND);
    r = call(0x16, sm, 0, 0, 0);
    ASSERT_EQ(r.x[0], HOS_RESULT_SUCCESS);
    r = call(0x21, sm, 0, 0, 0);
    ASSERT_EQ(r.x[0], HOS_KERNEL_INVALID_HANDLE);
}

TEST(ipc_sm_get_service_cmif_and_tipc) {
    init_once();
    uint32_t sm = connect_sm();
    ASSERT_TRUE(sm != 0);

    ipc_response resp;
    ipc_request req = {.type = IPC_TYPE_REQUEST, .cmd = 0, .send_pid = 1};
    ASSERT_EQ(ipc_call(sm, &req, &resp), 0);

    uint64_t name = service_name("lm");
    req = (ipc_request){.type = IPC_TYPE_REQUEST, .cmd = 1, .data = &name, .data_size = 8};
    ASSERT_EQ(ipc_call(sm, &req, &resp), 0);
    ASSERT_EQ(resp.num_move, 1);
    ASSERT_TRUE(resp.move_handles[0] != 0);
    ASSERT_EQ(call(0x16, resp.move_handles[0], 0, 0, 0).x[0], 0);

    name = service_name("absent");
    req = (ipc_request){.type = IPC_TYPE_REQUEST, .cmd = 1, .data = &name, .data_size = 8};
    ASSERT_EQ(ipc_call(sm, &req, &resp), HOS_RESULT_MAKE(21, 8));

    name = service_name("lm");
    req = (ipc_request){.type = IPC_TYPE_TIPC(1), .data = &name, .data_size = 8};
    ASSERT_EQ(ipc_call(sm, &req, &resp), 0);
    ASSERT_EQ(resp.num_move, 1);
    ASSERT_EQ(call(0x16, resp.move_handles[0], 0, 0, 0).x[0], 0);

    ASSERT_EQ(call(0x16, sm, 0, 0, 0).x[0], 0);
}

static uint32_t get_service(uint32_t sm, const char *name) {
    ipc_response resp;
    uint64_t n = service_name(name);
    ipc_request req = {.type = IPC_TYPE_REQUEST, .cmd = 1, .data = &n, .data_size = 8};
    if (ipc_call(sm, &req, &resp) != 0 || resp.num_move != 1) {
        return 0;
    }
    return resp.move_handles[0];
}

/* The services a save manager or a launcher asks for on a console with nothing
 * installed: the system account service, the application manager, play data. */
TEST(ipc_account_ns_pdm) {
    init_once();
    uint32_t sm = connect_sm();
    ipc_response resp;

    uint32_t acc = get_service(sm, "acc:su");
    ASSERT_TRUE(acc != 0);
    ipc_request req = {.type = IPC_TYPE_REQUEST, .cmd = 0}; /* GetUserCount */
    ASSERT_EQ(ipc_call(acc, &req, &resp), 0);
    ASSERT_EQ((int32_t)resp.data[0], 1);
    ASSERT_EQ(call(0x16, acc, 0, 0, 0).x[0], 0);

    uint32_t ns = get_service(sm, "ns:am2");
    ASSERT_TRUE(ns != 0);
    req = (ipc_request){.type = IPC_TYPE_REQUEST, .cmd = 7996}; /* GetApplicationManagerInterface */
    ASSERT_EQ(ipc_call(ns, &req, &resp), 0);
    ASSERT_EQ(resp.num_move, 1);
    uint32_t am = resp.move_handles[0];
    hos_svc_regs hr = call(0x01, 0, 0x200000, 0, 0); /* a 39-bit buffer for the records */
    ASSERT_EQ(hr.x[0], 0);
    int32_t offset = 0;
    req = (ipc_request){.type = IPC_TYPE_REQUEST,
                        .cmd = 0, /* ListApplicationRecord */
                        .data = &offset,
                        .data_size = 4,
                        .b_buf = (void *)hr.x[1],
                        .b_size = 0x100};
    ASSERT_EQ(ipc_call(am, &req, &resp), 0);
    ASSERT_EQ((int32_t)resp.data[0], 0);
    uint32_t storage = 5; /* SdCard */
    req = (ipc_request){.type = IPC_TYPE_REQUEST, .cmd = 47, .data = &storage, .data_size = 4};
    ASSERT_EQ(ipc_call(am, &req, &resp), 0); /* GetTotalSpaceSize */
    int64_t total;
    memcpy(&total, resp.data, 8);
    ASSERT_TRUE(total > 0);
    ASSERT_EQ(call(0x16, am, 0, 0, 0).x[0], 0);
    ASSERT_EQ(call(0x16, ns, 0, 0, 0).x[0], 0);

    uint32_t pdm = get_service(sm, "pdm:qry");
    ASSERT_TRUE(pdm != 0);
    uint64_t id = 0x0100000000010000ull;
    req = (ipc_request){.type = IPC_TYPE_REQUEST, .cmd = 4, .data = &id, .data_size = 8};
    ASSERT_EQ(ipc_call(pdm, &req, &resp), 0); /* QueryPlayStatisticsByApplicationId */
    ASSERT_EQ(resp.data_words, 0x28 / 4);
    uint64_t echoed;
    memcpy(&echoed, resp.data, 8);
    ASSERT_EQ(echoed, id);
    ASSERT_EQ(resp.data[0x24 / 4], 0); /* total_launches, the last word */
    ASSERT_EQ(call(0x16, pdm, 0, 0, 0).x[0], 0);
    ASSERT_EQ(call(0x16, sm, 0, 0, 0).x[0], 0);
}

/* The shared font: a shared memory handle, a loaded state and one font per type
 * (its size depends on whether the host has a TrueType font to serve). */
TEST(ipc_pl_shared_font) {
    init_once();
    uint32_t sm = connect_sm();
    uint32_t pl = get_service(sm, "pl:u");
    ASSERT_TRUE(pl != 0);
    ipc_response resp;
    ipc_request req = {.type = IPC_TYPE_REQUEST, .cmd = 4}; /* GetSharedMemoryNativeHandle */
    ASSERT_EQ(ipc_call(pl, &req, &resp), 0);
    ASSERT_EQ(resp.num_copy, 1);
    ASSERT_TRUE(resp.copy_handles[0] != 0);
    uint32_t type = 0;
    req = (ipc_request){.type = IPC_TYPE_REQUEST, .cmd = 0, .data = &type, .data_size = 4};
    ASSERT_EQ(ipc_call(pl, &req, &resp), 0); /* RequestLoad */
    req = (ipc_request){.type = IPC_TYPE_REQUEST, .cmd = 1, .data = &type, .data_size = 4};
    ASSERT_EQ(ipc_call(pl, &req, &resp), 0); /* GetLoadState */
    uint32_t loaded = resp.data[0];
    req = (ipc_request){.type = IPC_TYPE_REQUEST, .cmd = 2, .data = &type, .data_size = 4};
    uint32_t rc = ipc_call(pl, &req, &resp); /* GetSize */
    uint32_t size = 0;
    if (loaded) { /* the host has a font: a real size and offset 0 */
        ASSERT_EQ(rc, 0);
        size = resp.data[0];
        ASSERT_TRUE(size > 0 && size < 0x1100000);
        req = (ipc_request){.type = IPC_TYPE_REQUEST, .cmd = 3, .data = &type, .data_size = 4};
        ASSERT_EQ(ipc_call(pl, &req, &resp), 0); /* GetSharedMemoryAddressOffset */
        ASSERT_EQ(resp.data[0], 0);
    } else { /* no font: the size is an error, not a zero-length font */
        ASSERT_TRUE(rc != 0);
    }
    hos_svc_regs hr = call(0x01, 0, 0x200000, 0, 0);
    ASSERT_EQ(hr.x[0], 0);
    uint32_t *types = (uint32_t *)hr.x[1];
    uint64_t lang = 0;
    req = (ipc_request){.type = IPC_TYPE_REQUEST,
                        .cmd = 5, /* GetSharedFontInOrderOfPriority */
                        .data = &lang,
                        .data_size = 8,
                        .b_buf = types,
                        .b_size = 6 * 4};
    ASSERT_EQ(ipc_call(pl, &req, &resp), 0);
    ASSERT_EQ(resp.data[0] & 0xFF, 1);
    ASSERT_EQ(resp.data[1], size ? 6u : 0u);
    if (size) {
        ASSERT_EQ(types[5], 5);
    }
    ASSERT_EQ(call(0x16, pl, 0, 0, 0).x[0], 0);
    ASSERT_EQ(call(0x16, sm, 0, 0, 0).x[0], 0);
}

/* The built-in storage is an empty partition and there is no save data to list. */
TEST(ipc_fsp_bis_and_save_info) {
    init_once();
    uint32_t sm = connect_sm();
    uint32_t fsp = get_service(sm, "fsp-srv");
    ASSERT_TRUE(fsp != 0);
    ipc_response resp;
    hos_svc_regs hr = call(0x01, 0, 0x200000, 0, 0); /* 39-bit buffers for the paths */
    ASSERT_EQ(hr.x[0], 0);
    char *path = (char *)hr.x[1];
    memset(path, 0, 0x301);
    path[0] = '/';

    uint32_t partition = 30; /* User */
    ipc_request req = {.type = IPC_TYPE_REQUEST,
                       .cmd = 11, /* OpenBisFileSystem */
                       .data = &partition,
                       .data_size = 4,
                       .x_buf = path,
                       .x_size = 0x301};
    ASSERT_EQ(ipc_call(fsp, &req, &resp), 0);
    ASSERT_EQ(resp.num_move, 1);
    uint32_t fs = resp.move_handles[0];
    uint32_t mode = 3; /* directories and files */
    req = (ipc_request){.type = IPC_TYPE_REQUEST,
                        .cmd = 9, /* OpenDirectory */
                        .data = &mode,
                        .data_size = 4,
                        .x_buf = path,
                        .x_size = 0x301};
    ASSERT_EQ(ipc_call(fs, &req, &resp), 0);
    ASSERT_EQ(resp.num_move, 1);
    uint32_t dir = resp.move_handles[0];
    req = (ipc_request){.type = IPC_TYPE_REQUEST, .cmd = 1}; /* GetEntryCount */
    ASSERT_EQ(ipc_call(dir, &req, &resp), 0);
    int64_t count;
    memcpy(&count, resp.data, 8);
    ASSERT_EQ(count, 0);
    ASSERT_EQ(call(0x16, dir, 0, 0, 0).x[0], 0);
    ASSERT_EQ(call(0x16, fs, 0, 0, 0).x[0], 0);

    uint32_t space = 1; /* User */
    req = (ipc_request){.type = IPC_TYPE_REQUEST, .cmd = 61, .data = &space, .data_size = 4};
    ASSERT_EQ(ipc_call(fsp, &req, &resp), 0); /* OpenSaveDataInfoReaderBySaveDataSpaceId */
    ASSERT_EQ(resp.num_move, 1);
    uint32_t reader = resp.move_handles[0];
    req = (ipc_request){.type = IPC_TYPE_REQUEST, .cmd = 0, .b_buf = path, .b_size = 0x100};
    ASSERT_EQ(ipc_call(reader, &req, &resp), 0); /* ReadSaveDataInfo */
    memcpy(&count, resp.data, 8);
    ASSERT_EQ(count, 0);
    ASSERT_EQ(call(0x16, reader, 0, 0, 0).x[0], 0);
    ASSERT_EQ(call(0x16, fsp, 0, 0, 0).x[0], 0);
    ASSERT_EQ(call(0x16, sm, 0, 0, 0).x[0], 0);
}

static size_t make_log_packet(uint8_t *out, const char *text) {
    size_t n = strlen(text);
    memset(out, 0, 0x18);
    out[0x10] = 3;
    out[0x12] = 1;
    uint32_t payload = (uint32_t)(2 + n);
    memcpy(out + 0x14, &payload, 4);
    out[0x18] = 2;
    out[0x19] = (uint8_t)n;
    memcpy(out + 0x1A, text, n);
    return 0x1A + n;
}

TEST(ipc_lm_session_and_domain) {
    init_once();
    uint32_t sm = connect_sm();
    ipc_response resp;
    uint64_t name = service_name("lm");
    ipc_request req = {.type = IPC_TYPE_REQUEST, .cmd = 1, .data = &name, .data_size = 8};
    ASSERT_EQ(ipc_call(sm, &req, &resp), 0);
    uint32_t lm = resp.move_handles[0];

    /* plain session */
    req = (ipc_request){.type = IPC_TYPE_REQUEST, .cmd = 0, .send_pid = 1};
    ASSERT_EQ(ipc_call(lm, &req, &resp), 0);
    ASSERT_EQ(resp.num_move, 1);
    uint32_t logger = resp.move_handles[0];

    /* HIPC descriptors carry 39-bit addresses. Under qemu-user this test binary
     * and its stack live above that, so the packet goes in the HOS heap. */
    hos_svc_regs hr = call(0x01, 0, 0x200000, 0, 0);
    ASSERT_EQ(hr.x[0], 0);
    uint8_t *packet = (uint8_t *)hr.x[1];
    size_t len = make_log_packet(packet, "from test_ipc");
    req = (ipc_request){.type = IPC_TYPE_REQUEST, .cmd = 0, .a_buf = packet, .a_size = len};
    ASSERT_EQ(ipc_call(logger, &req, &resp), 0);
    req = (ipc_request){.type = IPC_TYPE_REQUEST, .cmd = 99};
    ASSERT_EQ(ipc_call(logger, &req, &resp), HOS_KERNEL_NOT_IMPLEMENTED);
    ASSERT_EQ(call(0x16, logger, 0, 0, 0).x[0], 0);

    /* control: pointer buffer size */
    req = (ipc_request){.type = IPC_TYPE_CONTROL, .cmd = 3};
    ASSERT_EQ(ipc_call(lm, &req, &resp), 0);
    ASSERT_EQ(resp.data[0], 0x8000);

    /* domain */
    req = (ipc_request){.type = IPC_TYPE_CONTROL, .cmd = 0};
    ASSERT_EQ(ipc_call(lm, &req, &resp), 0);
    ASSERT_EQ(resp.data[0], 1);
    req = (ipc_request){.type = IPC_TYPE_CONTROL, .cmd = 0};
    ASSERT_EQ(ipc_call(lm, &req, &resp), HOS_KERNEL_INVALID_STATE);

    req = (ipc_request){.type = IPC_TYPE_REQUEST,
                        .cmd = 0,
                        .send_pid = 1,
                        .domain = 1,
                        .domain_cmd = 1,
                        .object_id = 1};
    ASSERT_EQ(ipc_call(lm, &req, &resp), 0);
    ASSERT_EQ(resp.num_objects, 1);
    ASSERT_EQ(resp.object_ids[0], 2);
    ASSERT_EQ(resp.num_move, 0);

    len = make_log_packet(packet, "from test_ipc (domain)");
    req = (ipc_request){.type = IPC_TYPE_REQUEST,
                        .cmd = 0,
                        .a_buf = packet,
                        .a_size = len,
                        .domain = 1,
                        .domain_cmd = 1,
                        .object_id = 2};
    ASSERT_EQ(ipc_call(lm, &req, &resp), 0);

    req = (ipc_request){.type = IPC_TYPE_REQUEST, .domain = 1, .domain_cmd = 1, .object_id = 7};
    ASSERT_EQ(ipc_call(lm, &req, &resp), HOS_KERNEL_NOT_FOUND);

    req = (ipc_request){.type = IPC_TYPE_REQUEST, .domain = 1, .domain_cmd = 2, .object_id = 2};
    ASSERT_EQ(ipc_call(lm, &req, &resp), 0);

    /* CopyFromCurrentDomain hands out a session for the root object */
    uint32_t id = 1;
    req = (ipc_request){.type = IPC_TYPE_CONTROL, .cmd = 1, .data = &id, .data_size = 4};
    ASSERT_EQ(ipc_call(lm, &req, &resp), 0);
    ASSERT_EQ(resp.num_move, 1);
    ASSERT_EQ(call(0x16, resp.move_handles[0], 0, 0, 0).x[0], 0);

    ASSERT_EQ(call(0x16, lm, 0, 0, 0).x[0], 0);
    ASSERT_EQ(call(0x16, sm, 0, 0, 0).x[0], 0);
}

TEST(ipc_malformed_messages) {
    init_once();
    uint32_t sm = connect_sm();
    uint32_t *tls = hos_cpu_current_tls();
    memset(tls, 0, 0x100);
    tls[0] = IPC_TYPE_REQUEST | (9u << 20); /* 9 A descriptors: too many */
    hos_svc_regs r = call(0x21, sm, 0, 0, 0);
    ASSERT_EQ(r.x[0], HOS_KERNEL_INVALID_COMBINATION);

    memset(tls, 0, 0x100);
    tls[0] = IPC_TYPE_REQUEST;
    tls[1] = 8; /* raw data without SFCI */
    r = call(0x21, sm, 0, 0, 0);
    ASSERT_EQ(r.x[0], HOS_KERNEL_INVALID_COMBINATION);

    memset(tls, 0, 0x100);
    tls[0] = IPC_TYPE_CLOSE;
    r = call(0x21, sm, 0, 0, 0);
    ASSERT_EQ(r.x[0], HOS_KERNEL_SESSION_CLOSED);
    ASSERT_EQ(call(0x16, sm, 0, 0, 0).x[0], 0);
}

/*
 * ssl: the TLS service, as far as "the connection failed". Contexts and
 * connections are created and configured (socket, host name, verify and I/O
 * options are kept), but there is no TLS behind them yet: DoHandshake fails
 * with an ssl result, so curl and the apps built on it report an error and
 * carry on offline instead of stopping at a missing service. A real
 * implementation over the host's TLS library goes behind the same objects.
 */

#include <string.h>

#include "common/hos_log.h"
#include "hos_ipc.h"

#define MOD "ssl"

/* Module 123 (ssl); the apps only need a failure that is not "not implemented". */
#define SSL_RESULT_HANDSHAKE_FAILED HOS_RESULT_MAKE(123, 1500)

#define MAX_CONNECTIONS 16
#define HOSTNAME_MAX    256

typedef struct ssl_connection {
    int used;
    int32_t sockfd;
    uint32_t verify_option;
    uint32_t io_mode;
    uint32_t session_cache_mode;
    char hostname[HOSTNAME_MAX];
} ssl_connection;

static ssl_connection g_connections[MAX_CONNECTIONS];
static uint32_t g_context_count;

static const hos_ipc_buffer *any_in(const hos_ipc_ctx *ctx) {
    return ctx->num_a && ctx->a[0].addr   ? &ctx->a[0]
           : ctx->num_x && ctx->x[0].addr ? &ctx->x[0]
                                          : NULL;
}

static const hos_ipc_buffer *any_out(const hos_ipc_ctx *ctx) {
    return ctx->num_b && ctx->b[0].addr   ? &ctx->b[0]
           : ctx->num_c && ctx->c[0].addr ? &ctx->c[0]
                                          : NULL;
}

/* ---- ISslConnection ------------------------------------------------------------------- */

static hos_result connection_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    ssl_connection *c = obj->ctx;
    switch (ctx->cmd) {
    case 0: /* SetSocketDescriptor(s32) -> s32 */
        hos_ipc_in_read(ctx, 0, &c->sockfd, 4);
        hos_ipc_out_write(ctx, &c->sockfd, 4);
        return HOS_RESULT_SUCCESS;
    case 1: /* SetHostName(buffer) */ {
        const hos_ipc_buffer *b = any_in(ctx);
        memset(c->hostname, 0, sizeof(c->hostname));
        if (b && b->size) {
            size_t n = b->size < HOSTNAME_MAX - 1 ? b->size : HOSTNAME_MAX - 1;
            memcpy(c->hostname, (const void *)b->addr, n);
        }
        return HOS_RESULT_SUCCESS;
    }
    case 2: /* SetVerifyOption(u32) */
        hos_ipc_in_read(ctx, 0, &c->verify_option, 4);
        return HOS_RESULT_SUCCESS;
    case 3: /* SetIoMode(u32) */
        hos_ipc_in_read(ctx, 0, &c->io_mode, 4);
        return HOS_RESULT_SUCCESS;
    case 4: /* GetSocketDescriptor -> s32 */
        hos_ipc_out_write(ctx, &c->sockfd, 4);
        return HOS_RESULT_SUCCESS;
    case 5: /* GetHostName(out buffer) -> u32 length */ {
        const hos_ipc_buffer *b = any_out(ctx);
        uint32_t n = (uint32_t)strlen(c->hostname);
        if (b && b->size) {
            memset((void *)b->addr, 0, b->size);
            memcpy((void *)b->addr, c->hostname, n < b->size ? n : b->size);
        }
        hos_ipc_out_write(ctx, &n, 4);
        return HOS_RESULT_SUCCESS;
    }
    case 6: /* GetVerifyOption -> u32 */
        hos_ipc_out_write(ctx, &c->verify_option, 4);
        return HOS_RESULT_SUCCESS;
    case 7: /* GetIoMode -> u32 */
        hos_ipc_out_write(ctx, &c->io_mode, 4);
        return HOS_RESULT_SUCCESS;
    case 8:  /* DoHandshake */
    case 9:  /* DoHandshakeGetServerCert -> u32 size, u32 certs; out buffer */
    case 10: /* Read(out buffer) -> u32 */
    case 11: /* Write(buffer) -> u32 */
    case 13: /* Peek(out buffer) -> u32 */
        HOS_LOGW(MOD, "no TLS yet: %s to %s fails", ctx->cmd <= 9 ? "handshake" : "transfer",
                 c->hostname[0] ? c->hostname : "(no host name)");
        return SSL_RESULT_HANDSHAKE_FAILED;
    case 12: /* Pending -> s32 */
    case 14: /* Poll(u32 events, u32 timeout) -> u32 events */ {
        uint32_t zero = 0;
        hos_ipc_out_write(ctx, &zero, 4);
        return HOS_RESULT_SUCCESS;
    }
    case 15: /* GetVerifyCertError */
        return HOS_RESULT_SUCCESS;
    case 16: /* GetNeededServerCertBufferSize -> u32 */ {
        uint32_t zero = 0;
        hos_ipc_out_write(ctx, &zero, 4);
        return HOS_RESULT_SUCCESS;
    }
    case 17: /* SetSessionCacheMode(u32) */
        hos_ipc_in_read(ctx, 0, &c->session_cache_mode, 4);
        return HOS_RESULT_SUCCESS;
    case 18: /* GetSessionCacheMode -> u32 */
        hos_ipc_out_write(ctx, &c->session_cache_mode, 4);
        return HOS_RESULT_SUCCESS;
    case 19: /* FlushSessionCache */
    case 20: /* SetRenegotiationMode(u32) */
    case 22: /* GetRenegotiationMode? */
        return HOS_RESULT_SUCCESS;
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

static void connection_destroy(hos_object *obj) {
    ssl_connection *c = obj->ctx;
    c->used = 0;
}

static const hos_service_class g_connection_class = {
    .name = "ssl:ISslConnection",
    .dispatch = connection_dispatch,
    .destroy = connection_destroy,
};

/* ---- ISslContext ---------------------------------------------------------------------- */

static hos_result context_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case 0: /* SetOption(u32 option, s32 value) */
    case 5: /* AddPolicyOid(buffer) */
    case 7: /* RemovePki(u64 id) */
        return HOS_RESULT_SUCCESS;
    case 1: /* GetOption(u32 option) -> s32 */ {
        int32_t v = 0;
        hos_ipc_out_write(ctx, &v, 4);
        return HOS_RESULT_SUCCESS;
    }
    case 2: /* CreateConnection -> ISslConnection */ {
        ssl_connection *c = NULL;
        for (int i = 0; i < MAX_CONNECTIONS; i++) {
            if (!g_connections[i].used) {
                c = &g_connections[i];
                memset(c, 0, sizeof(*c));
                c->used = 1;
                c->sockfd = -1;
                break;
            }
        }
        if (!c) {
            return HOS_KERNEL_OUT_OF_RESOURCE;
        }
        hos_object *o = hos_ipc_object_new(&g_connection_class, c);
        if (!o) {
            c->used = 0;
            return HOS_KERNEL_OUT_OF_RESOURCE;
        }
        return hos_ipc_return_object(ctx, o);
    }
    case 3: /* GetConnectionCount -> u32 */ {
        uint32_t n = 0;
        for (int i = 0; i < MAX_CONNECTIONS; i++) {
            n += g_connections[i].used ? 1 : 0;
        }
        hos_ipc_out_write(ctx, &n, 4);
        return HOS_RESULT_SUCCESS;
    }
    case 4: /* ImportServerPki(u32 format, buffer) -> u64 id */
    case 6: /* ImportCrl(buffer) -> u64 id */
    case 8: /* RegisterInternalPki(u32) -> u64 id */ {
        uint64_t id = 1;
        hos_ipc_out_write(ctx, &id, 8);
        return HOS_RESULT_SUCCESS;
    }
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

static void context_destroy(hos_object *obj) {
    (void)obj;
    if (g_context_count) {
        g_context_count--;
    }
}

static const hos_service_class g_context_class = {
    .name = "ssl:ISslContext",
    .dispatch = context_dispatch,
    .destroy = context_destroy,
};

/* ---- ISslService ----------------------------------------------------------------------- */

static hos_result ssl_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case 0:   /* CreateContext(u32 version, u64 pid, pid) -> ISslContext */
    case 100: /* CreateContextForSystem */
        g_context_count++;
        return hos_ipc_return_object(ctx, hos_ipc_object_new(&g_context_class, NULL));
    case 1: /* GetContextCount -> u32 */
        hos_ipc_out_write(ctx, &g_context_count, 4);
        return HOS_RESULT_SUCCESS;
    case 2: /* GetCertificates(ids buffer; out buffer) -> u32 count */
    case 3: /* GetCertificateBufSize(ids buffer) -> u32 */ {
        uint32_t zero = 0;
        hos_ipc_out_write(ctx, &zero, 4);
        return HOS_RESULT_SUCCESS;
    }
    case 5: /* SetInterfaceVersion(u32) */
    case 7: /* ClearTls12FallbackFlag */
    case 8: /* SetThreadCoreMask(u64) */
        return HOS_RESULT_SUCCESS;
    case 9: /* GetThreadCoreMask -> u64 */ {
        uint64_t mask = 0;
        hos_ipc_out_write(ctx, &mask, 8);
        return HOS_RESULT_SUCCESS;
    }
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

const hos_service_class hos_ssl_class = {
    .name = "ssl",
    .dispatch = ssl_dispatch,
    .destroy = NULL,
};

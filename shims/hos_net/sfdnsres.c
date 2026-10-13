/*
 * sfdnsres: name resolution over the host resolver. libnx's getaddrinfo sends
 * the host, the service and packed hints and gets back a packed addrinfo list
 * (big-endian fields, FreeBSD sockaddr_in, canonical name, zero terminator),
 * the format of nnSdk's resolver; the host's getaddrinfo answers and the
 * results are packed the same way. Only IPv4 comes back, which is all bsd:u
 * serves. The rest of the interface is the string tables and a cancel handle.
 */

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>

#include "common/hos_log.h"
#include "hos_ipc.h"

#define MOD "dns"

#define ADDRINFO_MAGIC 0xBEEFCAFEu

/* FreeBSD numbering, what libnx's netdb.h defines. */
#define NX_AI_PASSIVE     0x01
#define NX_AI_CANONNAME   0x02
#define NX_AI_NUMERICHOST 0x04
#define NX_AI_NUMERICSERV 0x08
#define NX_EAI_AGAIN      2
#define NX_EAI_FAIL       4
#define NX_EAI_FAMILY     5
#define NX_EAI_MEMORY     6
#define NX_EAI_NONAME     8
#define NX_EAI_SERVICE    9
#define NX_EAI_SOCKTYPE   10

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static const hos_ipc_buffer *in_buffer(const hos_ipc_ctx *ctx, uint32_t n) {
    return n < ctx->num_a ? &ctx->a[n] : NULL;
}

static const hos_ipc_buffer *out_buffer(const hos_ipc_ctx *ctx, uint32_t n) {
    return n < ctx->num_b ? &ctx->b[n] : NULL;
}

/* A nul-terminated string from a buffer, or NULL when the buffer is empty. */
static const char *string_in(const hos_ipc_ctx *ctx, uint32_t n, char *tmp, size_t cap) {
    const hos_ipc_buffer *b = in_buffer(ctx, n);
    if (!b || !b->addr || !b->size) {
        return NULL;
    }
    size_t len = b->size < cap - 1 ? b->size : cap - 1;
    memcpy(tmp, (const void *)b->addr, len);
    tmp[len] = '\0';
    return tmp[0] ? tmp : NULL;
}

static int nx_flags_to_host(uint32_t f) {
    int out = 0;
    out |= (f & NX_AI_PASSIVE) ? AI_PASSIVE : 0;
    out |= (f & NX_AI_CANONNAME) ? AI_CANONNAME : 0;
    out |= (f & NX_AI_NUMERICHOST) ? AI_NUMERICHOST : 0;
    out |= (f & NX_AI_NUMERICSERV) ? AI_NUMERICSERV : 0;
    return out;
}

static uint32_t host_flags_to_nx(int f) {
    uint32_t out = 0;
    out |= (f & AI_PASSIVE) ? NX_AI_PASSIVE : 0;
    out |= (f & AI_CANONNAME) ? NX_AI_CANONNAME : 0;
    out |= (f & AI_NUMERICHOST) ? NX_AI_NUMERICHOST : 0;
    out |= (f & AI_NUMERICSERV) ? NX_AI_NUMERICSERV : 0;
    return out;
}

static int32_t gai_to_nx(int e) {
    switch (e) {
    case EAI_AGAIN:
        return NX_EAI_AGAIN;
    case EAI_NONAME:
#ifdef EAI_NODATA
    case EAI_NODATA:
#endif
        return NX_EAI_NONAME;
    case EAI_SERVICE:
        return NX_EAI_SERVICE;
    case EAI_FAMILY:
        return NX_EAI_FAMILY;
    case EAI_SOCKTYPE:
        return NX_EAI_SOCKTYPE;
    case EAI_MEMORY:
        return NX_EAI_MEMORY;
    default:
        return NX_EAI_FAIL;
    }
}

/* Pack one IPv4 result the way libnx unpacks it; returns the bytes it needs. */
static size_t pack_one(uint8_t *out, size_t room, const struct addrinfo *ai) {
    size_t name_len = ai->ai_canonname ? strlen(ai->ai_canonname) : 0;
    size_t need = 24 + 16 + name_len + 1;
    if (!out || need > room) {
        return need;
    }
    put_be32(out, ADDRINFO_MAGIC);
    put_be32(out + 4, host_flags_to_nx(ai->ai_flags));
    put_be32(out + 8, (uint32_t)ai->ai_family);
    put_be32(out + 12, (uint32_t)ai->ai_socktype);
    put_be32(out + 16, (uint32_t)ai->ai_protocol);
    put_be32(out + 20, 16);
    const struct sockaddr_in *sin = (const struct sockaddr_in *)ai->ai_addr;
    uint8_t *sa = out + 24;
    memset(sa, 0, 16);
    sa[0] = 16; /* sin_len */
    sa[1] = AF_INET;
    memcpy(sa + 2, &sin->sin_port, 2); /* network order on both sides */
    /* The address goes through htonl on the console side, so byte-reversed here. */
    const uint8_t *a = (const uint8_t *)&sin->sin_addr.s_addr;
    sa[4] = a[3];
    sa[5] = a[2];
    sa[6] = a[1];
    sa[7] = a[0];
    if (name_len) {
        memcpy(out + 40, ai->ai_canonname, name_len);
    }
    out[40 + name_len] = '\0';
    return need;
}

static hos_result get_addr_info(hos_ipc_ctx *ctx) {
    char host[256], service[64];
    const char *h = string_in(ctx, 0, host, sizeof(host));
    const char *s = string_in(ctx, 1, service, sizeof(service));
    const hos_ipc_buffer *hints_buf = in_buffer(ctx, 2);
    const hos_ipc_buffer *out = out_buffer(ctx, 0);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    if (hints_buf && hints_buf->addr && hints_buf->size >= 24 &&
        be32((const uint8_t *)hints_buf->addr) == ADDRINFO_MAGIC) {
        const uint8_t *p = (const uint8_t *)hints_buf->addr;
        hints.ai_flags = nx_flags_to_host(be32(p + 4));
        hints.ai_socktype = (int)be32(p + 12);
        hints.ai_protocol = (int)be32(p + 16);
    }

    struct {
        uint32_t errno_;
        int32_t ret;
        uint32_t size;
    } reply = {0, 0, 0};
    struct addrinfo *res = NULL;
    hos_ipc_blocking_begin();
    int e = getaddrinfo(h, s, &hints, &res);
    hos_ipc_blocking_end();
    if (e != 0) {
        reply.ret = gai_to_nx(e);
        reply.errno_ = e == EAI_SYSTEM ? (uint32_t)errno : 0;
        HOS_LOGI(MOD, "getaddrinfo(%s, %s): %s", h ? h : "", s ? s : "", gai_strerror(e));
        hos_ipc_out_write(ctx, &reply, sizeof(reply));
        return HOS_RESULT_SUCCESS;
    }
    uint8_t *dst = out && out->addr ? (uint8_t *)out->addr : NULL;
    size_t room = out ? out->size : 0;
    size_t used = 0;
    int count = 0;
    for (const struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        if (ai->ai_family != AF_INET || !ai->ai_addr) {
            continue;
        }
        size_t n = pack_one(dst ? dst + used : NULL, room > used ? room - used : 0, ai);
        if (!dst || used + n + 4 > room) {
            HOS_LOGW(MOD, "getaddrinfo(%s): results do not fit in %zu bytes", h ? h : "", room);
            break;
        }
        used += n;
        count++;
    }
    freeaddrinfo(res);
    if (dst && used + 4 <= room) {
        put_be32(dst + used, 0);
        used += 4;
    }
    if (count == 0) {
        reply.ret = NX_EAI_NONAME;
    }
    reply.size = (uint32_t)used;
    HOS_LOGD(MOD, "getaddrinfo(%s, %s): %d addresses in %zu bytes", h ? h : "", s ? s : "", count,
             used);
    hos_ipc_out_write(ctx, &reply, sizeof(reply));
    return HOS_RESULT_SUCCESS;
}

static hos_result string_out(hos_ipc_ctx *ctx, const char *text) {
    const hos_ipc_buffer *b = out_buffer(ctx, 0);
    if (b && b->addr && b->size) {
        memset((void *)b->addr, 0, b->size);
        strncpy((char *)b->addr, text, b->size - 1);
    }
    return HOS_RESULT_SUCCESS;
}

static hos_result sfdnsres_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case 6: /* GetAddrInfoRequest(u8 use_nsd, u32 cancel handle, u64 pid, pid; host, service,
             * hints; out buffer) -> u32 errno, s32 ret, u32 serialized size */
        return get_addr_info(ctx);
    case 8: /* GetHostStringErrorRequest(u32 err) -> buffer */
        return string_out(ctx, "host lookup failed");
    case 9: /* GetGaiStringErrorRequest(u32 err) -> buffer */ {
        uint32_t err = 0;
        hos_ipc_in_read(ctx, 0, &err, 4);
        return string_out(ctx, err == NX_EAI_NONAME ? "hostname nor servname provided, or not known"
                               : err == NX_EAI_AGAIN ? "temporary failure in name resolution"
                               : err == NX_EAI_SERVICE
                                   ? "servname not supported for ai_socktype"
                                   : "non-recoverable failure in name resolution");
    }
    case 10: /* RequestCancelHandle(u64 pid, pid) -> u32 handle */ {
        uint32_t h = 1;
        hos_ipc_out_write(ctx, &h, 4);
        return HOS_RESULT_SUCCESS;
    }
    case 11: /* CancelRequest(u32 handle, u64 pid, pid) */
        return HOS_RESULT_SUCCESS;
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

const hos_service_class hos_sfdnsres_class = {
    .name = "sfdnsres",
    .dispatch = sfdnsres_dispatch,
    .destroy = NULL,
};

/*
 * lm: the log manager. nn::diag and SDK printf end up here as binary log
 * packets. We decode the text fields and print them through hos_log, which is
 * how a retail binary gets to say anything at all.
 */

#include <string.h>

#include "common/hos_log.h"
#include "hos_ipc.h"

#define MOD "lm"

enum {
    LM_OPEN_LOGGER = 0,
};

enum {
    LOGGER_LOG = 0,
    LOGGER_SET_DESTINATION = 1,
};

/* LogPacketHeader, 0x18 bytes. */
typedef struct log_packet_header {
    uint64_t process_id;
    uint64_t thread_id;
    uint8_t flags;
    uint8_t pad;
    uint8_t severity;
    uint8_t verbosity;
    uint32_t payload_size;
} log_packet_header;

enum {
    LOG_KEY_TEXT = 2,
    LOG_KEY_LINE = 3,
    LOG_KEY_FILENAME = 4,
    LOG_KEY_FUNCTION = 5,
    LOG_KEY_MODULE = 6,
    LOG_KEY_THREAD = 7,
    LOG_KEY_PROCESS_NAME = 10,
};

static hos_log_level severity_level(uint8_t severity) {
    switch (severity) {
    case 0:
        return HOS_LOG_TRACE;
    case 1:
        return HOS_LOG_INFO;
    case 2:
        return HOS_LOG_WARN;
    default:
        return HOS_LOG_ERROR;
    }
}

static size_t read_uleb128(const uint8_t *p, size_t n, uint64_t *out) {
    uint64_t v = 0;
    int shift = 0;
    for (size_t i = 0; i < n && i < 10; i++) {
        v |= (uint64_t)(p[i] & 0x7F) << shift;
        shift += 7;
        if (!(p[i] & 0x80)) {
            *out = v;
            return i + 1;
        }
    }
    *out = v;
    return n;
}

static void log_packet(const uint8_t *p, size_t n) {
    log_packet_header h;
    if (n < sizeof(h)) {
        return;
    }
    memcpy(&h, p, sizeof(h));
    const uint8_t *f = p + sizeof(h);
    size_t left = n - sizeof(h);
    if (h.payload_size < left) {
        left = h.payload_size;
    }
    char text[1024];
    char module[64] = "";
    text[0] = '\0';
    while (left >= 2) {
        uint8_t key = f[0];
        uint64_t len;
        size_t used = 1 + read_uleb128(f + 1, left - 1, &len);
        if (used + len > left) {
            break;
        }
        const uint8_t *data = f + used;
        if (key == LOG_KEY_TEXT) {
            size_t l = len < sizeof(text) - 1 ? (size_t)len : sizeof(text) - 1;
            memcpy(text, data, l);
            text[l] = '\0';
            while (l > 0 && (text[l - 1] == '\n' || text[l - 1] == '\r')) {
                text[--l] = '\0';
            }
        } else if (key == LOG_KEY_MODULE) {
            size_t l = len < sizeof(module) - 1 ? (size_t)len : sizeof(module) - 1;
            memcpy(module, data, l);
            module[l] = '\0';
        }
        f += used + len;
        left -= used + len;
    }
    if (text[0]) {
        hos_log(severity_level(h.severity), module[0] ? module : "game", "%s", text);
    }
}

static hos_result logger_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case LOGGER_LOG: {
        const hos_ipc_buffer *b = ctx->num_a ? &ctx->a[0] : (ctx->num_x ? &ctx->x[0] : NULL);
        if (!b || !b->addr) {
            return HOS_KERNEL_INVALID_POINTER;
        }
        const uint8_t *p = (const uint8_t *)b->addr;
        size_t n = b->size;
        while (n >= sizeof(log_packet_header)) {
            log_packet_header h;
            memcpy(&h, p, sizeof(h));
            size_t pkt = sizeof(h) + h.payload_size;
            if (pkt > n) {
                pkt = n;
            }
            log_packet(p, pkt);
            p += pkt;
            n -= pkt;
        }
        return HOS_RESULT_SUCCESS;
    }
    case LOGGER_SET_DESTINATION:
        return HOS_RESULT_SUCCESS;
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

static const hos_service_class g_logger_class = {
    .name = "lm:ILogger",
    .dispatch = logger_dispatch,
    .destroy = NULL,
};

static hos_result lm_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case LM_OPEN_LOGGER:
        return hos_ipc_return_object(ctx, hos_ipc_object_new(&g_logger_class, NULL));
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

const hos_service_class hos_lm_class = {
    .name = "lm",
    .dispatch = lm_dispatch,
    .destroy = NULL,
};

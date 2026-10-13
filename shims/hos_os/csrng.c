/* csrng: random bytes from the host's getrandom; libnx seeds its generator from it. */

#include <sys/random.h>

#include "common/hos_log.h"
#include "hos_ipc.h"

#define MOD "csrng"

static hos_result csrng_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case 0: /* GenerateRandomBytes(out buffer) */ {
        const hos_ipc_buffer *b = ctx->num_b && ctx->b[0].addr   ? &ctx->b[0]
                                  : ctx->num_c && ctx->c[0].addr ? &ctx->c[0]
                                                                 : NULL;
        if (!b) {
            return HOS_KERNEL_INVALID_ADDRESS;
        }
        uint8_t *p = (uint8_t *)b->addr;
        uint64_t left = b->size;
        while (left) {
            ssize_t n = getrandom(p, left, 0);
            if (n <= 0) {
                return HOS_KERNEL_OUT_OF_RESOURCE;
            }
            p += n;
            left -= (uint64_t)n;
        }
        return HOS_RESULT_SUCCESS;
    }
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

const hos_service_class hos_csrng_class = {
    .name = "csrng",
    .dispatch = csrng_dispatch,
    .destroy = NULL,
};

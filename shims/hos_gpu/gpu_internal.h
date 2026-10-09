#ifndef HOS_GPU_INTERNAL_H
#define HOS_GPU_INTERNAL_H

#include <stdint.h>

#include "hos_ipc.h"

#define HOS_HIDDEN __attribute__((visibility("hidden")))

/* nvdrv.c: nvmap allocations are game memory; vi reads frames straight from them. */
HOS_HIDDEN int nv_map_lookup(uint32_t id, uint64_t *addr, uint64_t *size);

extern HOS_HIDDEN const hos_service_class hos_nvdrv_class;
extern HOS_HIDDEN const hos_service_class hos_vi_u_class;
extern HOS_HIDDEN const hos_service_class hos_vi_m_class;
extern HOS_HIDDEN const hos_service_class hos_vi_s_class;

/*
 * The index-th non-null input buffer (A then X) or output buffer (B then C).
 * With HipcAutoSelect the client sends both descriptor kinds and nulls the
 * one it did not use, so null entries are skipped.
 */
static inline const hos_ipc_buffer *gpu_pick_buffer(const hos_ipc_buffer *first, uint32_t nfirst,
                                                    const hos_ipc_buffer *second, uint32_t nsecond,
                                                    uint32_t index) {
    for (uint32_t i = 0; i < nfirst; i++) {
        if (first[i].addr && index-- == 0) {
            return &first[i];
        }
    }
    for (uint32_t i = 0; i < nsecond; i++) {
        if (second[i].addr && index-- == 0) {
            return &second[i];
        }
    }
    return NULL;
}

static inline const hos_ipc_buffer *gpu_in_buffer(const hos_ipc_ctx *ctx, uint32_t index) {
    return gpu_pick_buffer(ctx->a, ctx->num_a, ctx->x, ctx->num_x, index);
}

static inline const hos_ipc_buffer *gpu_out_buffer(const hos_ipc_ctx *ctx, uint32_t index) {
    return gpu_pick_buffer(ctx->b, ctx->num_b, ctx->c, ctx->num_c, index);
}

#endif /* HOS_GPU_INTERNAL_H */

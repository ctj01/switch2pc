#ifndef HOS_GPU_INTERNAL_H
#define HOS_GPU_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "hos_ipc.h"

#define HOS_HIDDEN __attribute__((visibility("hidden")))

/* nvdrv.c: nvmap allocations are game memory; vi reads frames straight from them. */
HOS_HIDDEN int nv_map_lookup(uint32_t id, uint64_t *addr, uint64_t *size);

/* nvgpu.c: the GPU devices (docs/gpu.md, milestone M0). */
enum { NV_GPU_DEV_CTRL = 1, NV_GPU_DEV_AS = 2, NV_GPU_DEV_CHANNEL = 3 };
HOS_HIDDEN uint32_t nv_gpu_ioctl(int device, uint32_t fd, uint32_t cmd, uint8_t *io, size_t n,
                                 const uint8_t *extra_in, size_t extra_in_n, uint8_t *extra_out,
                                 size_t extra_out_n);
HOS_HIDDEN uint32_t nv_syncpt_read(uint32_t id);
HOS_HIDDEN uint32_t nv_syncpt_reach(uint32_t id, uint32_t threshold);
HOS_HIDDEN void *nv_gpu_va_to_host(uint64_t va, uint64_t len);
HOS_HIDDEN void *nv_gpu_va_to_host_span(uint64_t va, uint64_t *avail);
HOS_HIDDEN void nv_gpu_channel_close(uint32_t fd);

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

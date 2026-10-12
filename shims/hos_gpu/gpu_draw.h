#ifndef HOS_GPU_DRAW_H
#define HOS_GPU_DRAW_H

#include <stdint.h>

/*
 * The 3D engine's draw path on the CPU (M3, docs/gpu.md): vertex shading,
 * primitive assembly, viewport transform, culling, triangle rasterisation
 * with perspective-correct interpolation, fragment shading and colour
 * write. Depth testing, blending and textures come in later steps.
 */

#define GPU_STAGES 5 /* vertex, tess control, tess eval, geometry, fragment */
#define GPU_CBUFS  18

typedef struct gpu_draw_state {
    const uint32_t *r; /* the 3D engine's register file */
    uint64_t cb_addr[GPU_STAGES][GPU_CBUFS];
    uint32_t cb_size[GPU_STAGES][GPU_CBUFS];
} gpu_draw_state;

typedef struct gpu_draw_stats {
    uint64_t draws;
    uint64_t triangles;
    uint64_t pixels;
    uint64_t shader_errors;
} gpu_draw_stats;

/** Draw `count` vertices from `first` with the topology of VERTEX_BEGIN_GL. */
void gpu_draw_arrays(const gpu_draw_state *s, uint32_t topology, uint32_t first, uint32_t count,
                     uint32_t instance, gpu_draw_stats *st);

/** CLEAR_SURFACE's depth (bit 0) and stencil (bit 1) part, on the bound depth target. */
void gpu_clear_depth_stencil(const uint32_t *r, uint32_t surface_bits);

/* Shared with gpu_engines.c. */
uint32_t gpu_encode_color(uint32_t format, const float rgba[4], uint8_t out[16]);
/** Inverse of gpu_encode_color; returns bytes per pixel, 0 if the format is unknown. */
uint32_t gpu_decode_color(uint32_t format, const uint8_t *in, float rgba[4]);
uint64_t gpu_bl_offset(uint32_t x_bytes, uint32_t y, uint32_t width_bytes, uint32_t block_h_log2);
uint64_t gpu_bl_size(uint32_t width_bytes, uint32_t height, uint32_t block_h_log2);

#endif /* HOS_GPU_DRAW_H */

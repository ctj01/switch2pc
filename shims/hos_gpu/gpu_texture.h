#ifndef HOS_GPU_TEXTURE_H
#define HOS_GPU_TEXTURE_H

#include <stdint.h>

#include "gpu_shader.h"

/*
 * Textures for the CPU draw path (M4, docs/gpu.md): a Maxwell image
 * descriptor (TIC) and sampler descriptor (TSC) decoded into one struct, texel
 * fetches from any mip level, and sampling with the level of detail the
 * coordinate derivatives give. Field layouts follow the public Maxwell texture
 * header documentation (envytools gm200_texture.xml), as deko3d encodes them:
 *
 *   TIC word 0 format [6:0], swizzle [30:19]; words 1-2 address; word 2
 *   [23:21] layout (2 pitch, 3 block-linear); word 3 block height [5:3] or
 *   pitch >> 5 [15:0], mip levels - 1 [31:28]; word 4 width - 1 [15:0] and
 *   texture type [26:23] (7: no mipmaps); word 5 height - 1 [15:0]; word 6 lod
 *   bias [18:6] (s8.5 in 1/256); word 7 view min level [3:0], max level [7:4].
 *   TSC word 0 wrap u [2:0], v [5:3]; word 1 mag [1:0], min [5:4], mip [7:6]
 *   filters (1 nearest, 2 linear; mip 1 none, 2 nearest, 3 linear) and lod
 *   bias [24:12] (s4.8); word 2 min [11:0] and max [23:12] lod (u4.8); words
 *   4-7 the border colour.
 *
 * Mip levels follow level 0 in memory, each a block-linear surface of its own
 * whose block height shrinks with the level the way deko3d lays them out
 * (DkImageLayout::calcLevelOffset); a pitch-linear image has one level.
 */

#define GPU_TEX_MAX_LEVELS 16

/* Resolve a GPU address range to host memory; NULL when it is not mapped. */
typedef const void *(*gpu_tex_map_fn)(void *user, uint64_t va, uint64_t size);

typedef struct gpu_texture {
    int valid;           /* decoded, every level mapped */
    uint32_t format;     /* TIC format code */
    uint32_t swizzle;    /* TIC component sources, 3 bits per output */
    uint32_t block;      /* texels per side of a compressed block, else 1 */
    uint32_t unit_bytes; /* bytes per texel, or per block */
    int pitch_layout;
    uint32_t width, height;        /* level 0, in texels */
    uint32_t levels;               /* levels present in memory */
    uint32_t min_level, max_level; /* the view's range, inclusive */
    const uint8_t *base;           /* level 0; the chain is contiguous */
    uint64_t size;                 /* the whole chain */
    uint64_t level_off[GPU_TEX_MAX_LEVELS];
    uint32_t level_row_bytes[GPU_TEX_MAX_LEVELS]; /* bytes per row of units (unaligned) */
    uint8_t level_bh[GPU_TEX_MAX_LEVELS];         /* block height log2 of that level */
    uint32_t wrap_u, wrap_v;
    uint32_t mag_filter, min_filter, mip_filter;
    float lod_bias, lod_min, lod_max; /* sampler bias plus the image's, and the clamps */
    float border[4];
} gpu_texture;

/**
 * Decode a TIC/TSC pair. `map` resolves the image's memory (all levels at once);
 * returns 1 when the texture can be sampled, 0 otherwise (unknown format or
 * layout, or memory not mapped), with `t->format` filled in either way.
 */
int gpu_tex_init(gpu_texture *t, const uint32_t tic[8], const uint32_t tsc[8], gpu_tex_map_fn map,
                 void *user);

/** Bytes per texel, or per 4x4 block for the compressed formats; 0 if unknown. */
uint32_t gpu_tex_unit_bytes(uint32_t format);

/** Size of one level in texels. */
uint32_t gpu_tex_level_width(const gpu_texture *t, uint32_t level);
uint32_t gpu_tex_level_height(const gpu_texture *t, uint32_t level);

/**
 * The level of detail for a sample, after the sampler's bias and clamps and
 * relative to the view's first level: <= 0 magnifies, otherwise the mip filter
 * picks or blends levels. Without derivatives an implicit lod is 0.
 */
float gpu_tex_lambda(const gpu_texture *t, const shader_tex_coord *c);

/** Sample with normalised coordinates: wrap modes, filters and mip levels applied. */
void gpu_tex_sample(const gpu_texture *t, const shader_tex_coord *c, float out[4]);

/** texelFetch: integer texel of `level` (relative to the view's first), wrapped and swizzled. */
void gpu_tex_fetch(const gpu_texture *t, uint32_t level, int32_t x, int32_t y, float out[4]);

/**
 * One texel of a block-compressed 4x4 block: BC1 (0x24, 8 bytes), BC2 (0x25,
 * 16), BC3 (0x26, 16), BC4 (0x27, 8: red only) and BC5 (0x28, 16: red and
 * green), unsigned. `tx`, `ty` in 0..3. Colours as floats in 0..1.
 */
void gpu_bc_texel(uint32_t format, const uint8_t *block, uint32_t tx, uint32_t ty, float c[4]);

#endif /* HOS_GPU_TEXTURE_H */

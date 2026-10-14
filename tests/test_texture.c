/*
 * Textures (M4, docs/gpu.md): block-compressed texels decoded against values
 * worked out by hand from the formats' definitions, the mip chain laid out as
 * deko3d does, and the level of detail picking and blending levels the way the
 * OpenGL rules say. The image memory is a buffer this test owns; the TIC's
 * address is its offset into that buffer. Descriptors are written through
 * deko3d's own field layout (TextureImageControl, texture_image_control_block.h)
 * and texels are placed with a block-linear swizzle written out from the GOB
 * definition, independent of swizzle.c.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_texture.h"
#include "test.h"

enum {
    FMT_RGBA8 = 0x08,
    FMT_BC1 = 0x24,
    FMT_BC2 = 0x25,
    FMT_BC3 = 0x26,
    FMT_BC4 = 0x27,
    FMT_BC5 = 0x28
};
enum { NEAREST = 1, LINEAR = 2, MIP_NONE = 1, MIP_NEAREST = 2, MIP_LINEAR = 3 };

static uint8_t g_mem[0x20000] __attribute__((aligned(64)));

static const void *map(void *user, uint64_t va, uint64_t size) {
    (void)user;
    return va + size <= sizeof(g_mem) ? g_mem + va : NULL;
}

static int near(float a, float b) {
    return fabsf(a - b) < 1.0f / 512.0f;
}

/* deko3d's TextureImageControl, field for field (bit fields fill from the low
 * end on the compilers that build both deko3d and this test). */
typedef struct tic_fields {
    uint32_t format : 7, r_type : 3, g_type : 3, b_type : 3, a_type : 3, x_source : 3, y_source : 3,
        z_source : 3, w_source : 3, : 1;
    uint32_t address_low;
    uint32_t address_high : 16, view_layer_base_3_7 : 5, hdr_version : 3, load_store_hint_maybe : 1,
        view_coherency_hash : 4, view_layer_base_8_10 : 3;
    uint32_t tile_width_gobs_log2 : 3, tile_height_gobs_log2 : 3, tile_depth_gobs_log2 : 3, : 1,
        sparse_tile_width_gobs_log2 : 3, gob_3d : 1, : 2, lod_aniso_quality_2 : 1,
        lod_aniso_quality : 1, lod_iso_quality : 1, aniso_coarse_spread_modifier : 2,
        aniso_spread_scale : 5, use_header_opt_control : 1, depth_texture : 1, mip_max_levels : 4;
    uint32_t width_minus_one : 16, view_layer_base_0_2 : 3, aniso_spread_max_log2 : 3, is_srgb : 1,
        texture_type : 4, sector_promotion : 2, border_size : 3;
    uint32_t height_minus_one : 16, depth_minus_one : 14, is_sparse : 1, normalized_coords : 1;
    uint32_t color_key_op : 1, trilin_opt : 5, mip_lod_bias : 13, aniso_bias : 4,
        aniso_fine_spread_func : 2, aniso_coarse_spread_func : 2, max_anisotropy : 3,
        aniso_fine_spread_modifier : 2;
    uint32_t view_mip_min_level : 4, view_mip_max_level : 4, msaa_mode : 4, min_lod_clamp : 12, : 8;
} tic_fields;
_Static_assert(sizeof(tic_fields) == 32, "TextureImageControl is 32 bytes");

enum { TYPE_SNORM = 1, TYPE_UNORM = 2, TEXTURE_2D = 1, TEXTURE_2D_NO_MIPMAP = 7 };
enum { HDR_VERSION_BLOCKLINEAR = 3, HDR_VERSION_PITCH = 2 };

/* A 2D block-linear image at `va` with every level in view. */
static void make_tic(uint32_t tic[8], uint32_t format, uint32_t width, uint32_t height,
                     uint32_t bh_log2, uint32_t levels, uint64_t va) {
    tic_fields f;
    memset(&f, 0, sizeof(f));
    f.format = format;
    f.r_type = f.g_type = f.b_type = f.a_type = TYPE_UNORM;
    f.x_source = 2; /* R */
    f.y_source = 3;
    f.z_source = 4;
    f.w_source = 5;
    f.address_low = (uint32_t)va;
    f.address_high = (uint32_t)(va >> 32) & 0xFFFF;
    f.hdr_version = HDR_VERSION_BLOCKLINEAR;
    f.tile_height_gobs_log2 = bh_log2;
    f.mip_max_levels = levels - 1;
    f.width_minus_one = width - 1;
    f.texture_type = TEXTURE_2D;
    f.height_minus_one = height - 1;
    f.view_mip_max_level = levels - 1;
    memcpy(tic, &f, 32);
}

static tic_fields *tic_of(uint32_t tic[8]) {
    return (tic_fields *)tic;
}

static void make_tsc(uint32_t tsc[8], uint32_t mag, uint32_t min, uint32_t mip, float bias,
                     float lod_min, float lod_max) {
    memset(tsc, 0, 32);
    tsc[0] = 0; /* repeat */
    uint32_t b = (uint32_t)(int32_t)(bias * 256.0f) & 0x1FFF;
    tsc[1] = mag | (min << 4) | (mip << 6) | (b << 12);
    tsc[2] = (uint32_t)(lod_min * 256.0f) | ((uint32_t)(lod_max * 256.0f) << 12);
}

/* ---- block compression, against hand-decoded blocks ------------------------------------ */

/* Colour block: c0 = pure red 0xF800, c1 = pure blue 0x001F; indices 0 1 2 3 across row 0,
 * all 3 in row 1, all 2 in row 2, all 0 in row 3. c0 > c1: four colours. */
static const uint8_t k_color4[8] = {0x00, 0xF8, 0x1F, 0x00, 0xE4, 0xFF, 0xAA, 0x00};
/* The same endpoints the other way round (c0 < c1): three colours and transparent black. */
static const uint8_t k_color3[8] = {0x1F, 0x00, 0x00, 0xF8, 0xE4, 0xFF, 0xAA, 0x00};

TEST(bc1_four_and_three_colour_modes) {
    float c[4];
    gpu_bc_texel(FMT_BC1, k_color4, 0, 0, c); /* index 0: red */
    ASSERT_TRUE(near(c[0], 1.0f) && near(c[1], 0.0f) && near(c[2], 0.0f) && near(c[3], 1.0f));
    gpu_bc_texel(FMT_BC1, k_color4, 1, 0, c); /* index 1: blue */
    ASSERT_TRUE(near(c[0], 0.0f) && near(c[2], 1.0f));
    gpu_bc_texel(FMT_BC1, k_color4, 2, 0, c); /* index 2: 2/3 red + 1/3 blue */
    ASSERT_TRUE(near(c[0], 2.0f / 3.0f) && near(c[2], 1.0f / 3.0f));
    gpu_bc_texel(FMT_BC1, k_color4, 3, 0, c); /* index 3: 1/3 red + 2/3 blue */
    ASSERT_TRUE(near(c[0], 1.0f / 3.0f) && near(c[2], 2.0f / 3.0f));
    gpu_bc_texel(FMT_BC1, k_color4, 0, 1, c); /* row 1: all index 3 */
    ASSERT_TRUE(near(c[0], 1.0f / 3.0f) && near(c[2], 2.0f / 3.0f));
    gpu_bc_texel(FMT_BC1, k_color4, 3, 2, c); /* row 2: all index 2 */
    ASSERT_TRUE(near(c[0], 2.0f / 3.0f));

    gpu_bc_texel(FMT_BC1, k_color3, 2, 0, c); /* three-colour mode: index 2 is the average */
    ASSERT_TRUE(near(c[0], 0.5f) && near(c[2], 0.5f) && near(c[3], 1.0f));
    gpu_bc_texel(FMT_BC1, k_color3, 3, 0, c); /* index 3: transparent black */
    ASSERT_TRUE(near(c[0], 0.0f) && near(c[2], 0.0f) && near(c[3], 0.0f));
}

TEST(bc2_explicit_alpha_and_always_four_colours) {
    /* Alpha nibbles: texel (x, y) holds (x + 4y) & 15, LSB first in each byte. */
    uint8_t block[16];
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x += 2) {
            uint32_t lo = (uint32_t)(x + 4 * y) & 15, hi = (uint32_t)(x + 1 + 4 * y) & 15;
            block[2 * y + x / 2] = (uint8_t)(lo | hi << 4);
        }
    }
    memcpy(block + 8, k_color3, 8); /* c0 < c1, yet BC2 colours are always four */
    float c[4];
    gpu_bc_texel(FMT_BC2, block, 1, 2, c);
    ASSERT_TRUE(near(c[3], 9.0f / 15.0f));
    gpu_bc_texel(FMT_BC2, block, 3, 3, c);
    ASSERT_TRUE(near(c[3], 15.0f / 15.0f));
    gpu_bc_texel(FMT_BC2, block, 2, 0, c); /* index 2 with c0 = blue, c1 = red: 2/3 blue */
    ASSERT_TRUE(near(c[2], 2.0f / 3.0f) && near(c[0], 1.0f / 3.0f));
    gpu_bc_texel(FMT_BC2, block, 3, 0, c); /* index 3: 1/3 blue + 2/3 red, opaque */
    ASSERT_TRUE(near(c[2], 1.0f / 3.0f) && near(c[0], 2.0f / 3.0f) && near(c[3], 3.0f / 15.0f));
}

/* 3-bit alpha indices, LSB first: texel i gets index i & 7. */
static void alpha_indices(uint8_t *b) {
    uint64_t bits = 0;
    for (int i = 15; i >= 0; i--) {
        bits = (bits << 3) | (uint64_t)(i & 7);
    }
    for (int k = 0; k < 6; k++) {
        b[2 + k] = (uint8_t)(bits >> (8 * k));
    }
}

TEST(bc3_interpolated_alpha) {
    uint8_t block[16];
    memset(block, 0, sizeof(block));
    block[0] = 200; /* a0 > a1: eight interpolated values */
    block[1] = 60;
    alpha_indices(block);
    memcpy(block + 8, k_color4, 8);
    float c[4];
    /* texel i (x = i % 4, y = i / 4) has index i & 7 */
    static const float expect8[8] = {200.0f / 255, 60.0f / 255,  180.0f / 255, 160.0f / 255,
                                     140.0f / 255, 120.0f / 255, 100.0f / 255, 80.0f / 255};
    for (int i = 0; i < 16; i++) {
        gpu_bc_texel(FMT_BC3, block, (uint32_t)i % 4, (uint32_t)i / 4, c);
        ASSERT_TRUE(near(c[3], expect8[i & 7]));
    }
    gpu_bc_texel(FMT_BC3, block, 0, 0, c); /* colours decode as in BC1's four-colour mode */
    ASSERT_TRUE(near(c[0], 1.0f) && near(c[2], 0.0f));

    block[0] = 60; /* a0 <= a1: six values plus 0 and 1 */
    block[1] = 200;
    static const float expect6[8] = {60.0f / 255,  200.0f / 255, 88.0f / 255, 116.0f / 255,
                                     144.0f / 255, 172.0f / 255, 0.0f,        1.0f};
    for (int i = 0; i < 16; i++) {
        gpu_bc_texel(FMT_BC3, block, (uint32_t)i % 4, (uint32_t)i / 4, c);
        ASSERT_TRUE(near(c[3], expect6[i & 7]));
    }
}

TEST(bc4_bc5_single_channels) {
    uint8_t block[16];
    memset(block, 0, sizeof(block));
    block[0] = 255; /* a0 > a1 */
    block[1] = 0;
    alpha_indices(block);
    block[8] = 0; /* second channel, a0 <= a1 */
    block[9] = 255;
    alpha_indices(block + 8);
    float c[4];
    gpu_bc_texel(FMT_BC4, block, 2, 0, c); /* index 2: 6/7 */
    ASSERT_TRUE(near(c[0], 6.0f / 7.0f) && near(c[1], 0.0f) && near(c[2], 0.0f) &&
                near(c[3], 1.0f));
    gpu_bc_texel(FMT_BC5, block, 2, 0, c); /* red 6/7; green index 2 of (0, 255): 255/5 */
    ASSERT_TRUE(near(c[0], 6.0f / 7.0f) && near(c[1], 1.0f / 5.0f) && near(c[2], 0.0f));
    gpu_bc_texel(FMT_BC5, block, 2, 1, c); /* texel 6: red index 6 = 2/7, green 0 */
    ASSERT_TRUE(near(c[0], 2.0f / 7.0f) && near(c[1], 0.0f));
    gpu_bc_texel(FMT_BC5, block, 3, 1, c); /* texel 7: red 1/7, green 1 */
    ASSERT_TRUE(near(c[0], 1.0f / 7.0f) && near(c[1], 1.0f));
}

/* ---- the mip chain ------------------------------------------------------------------- */

/*
 * Block-linear addressing written out from the layout's definition, not from
 * swizzle.c: a GOB is 64 bytes by 8 rows (512 bytes), laid out in 16-byte
 * sectors as ((x / 32) * 256 + (y / 2) * 64 + ((x % 32) / 16) * 32 + (y % 2) *
 * 16 + x % 16) within it; a block stacks 2^bh GOBs; blocks tile the surface
 * row by row, with rows padded to whole GOBs.
 */
static uint64_t ref_offset(uint32_t x, uint32_t y, uint32_t row_bytes, uint32_t bh_log2) {
    uint32_t block_rows = 8u << bh_log2, gobs_per_row = (row_bytes + 63) / 64;
    uint64_t block = (uint64_t)(y / block_rows) * gobs_per_row + x / 64;
    uint64_t gob = block * (1u << bh_log2) + (y % block_rows) / 8;
    uint32_t xx = x % 64, yy = y % 8;
    return gob * 512 + (xx / 32) * 256 + (yy / 2) * 64 + ((xx % 32) / 16) * 32 + (yy % 2) * 16 +
           xx % 16;
}

/* Every texel of a level is written with its own address: R = 51 * level,
 * G = 16 * x, B = 16 * y, A = 255 (levels up to 4, 16 texels a side). */
/* Where a level starts in g_mem (the descriptor's address plus the level's offset). */
static uint8_t *level_mem(const gpu_texture *t, uint32_t level) {
    return g_mem + (t->base - g_mem) + t->level_off[level];
}

static void fill_level(const gpu_texture *t, uint32_t level) {
    uint32_t w = gpu_tex_level_width(t, level), h = gpu_tex_level_height(t, level);
    uint32_t row_bytes = w * 4;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint8_t px[4] = {(uint8_t)(51 * level), (uint8_t)(16 * x), (uint8_t)(16 * y), 255};
            memcpy(level_mem(t, level) + ref_offset(4 * x, y, row_bytes, t->level_bh[level]), px,
                   4);
        }
    }
}

static void mip_texture(gpu_texture *t, uint32_t mag, uint32_t min, uint32_t mip, float bias,
                        float lod_min, float lod_max) {
    uint32_t tic[8], tsc[8];
    make_tic(tic, FMT_RGBA8, 16, 16, 1, 5, 0x100);
    make_tsc(tsc, mag, min, mip, bias, lod_min, lod_max);
    ASSERT_TRUE(gpu_tex_init(t, tic, tsc, map, NULL));
    for (uint32_t l = 0; l < 5; l++) {
        fill_level(t, l);
    }
}

/* The red channel names the level, whatever the texel. */
static int is_level_color(const float c[4], uint32_t level) {
    return near(c[0], 51.0f * (float)level / 255.0f) && near(c[3], 1.0f);
}

static int is_texel(const float c[4], uint32_t level, uint32_t x, uint32_t y) {
    return is_level_color(c, level) && near(c[1], 16.0f * (float)x / 255.0f) &&
           near(c[2], 16.0f * (float)y / 255.0f);
}

TEST(mip_chain_layout_as_deko3d) {
    gpu_texture t;
    mip_texture(&t, LINEAR, LINEAR, MIP_LINEAR, 0.0f, 0.0f, 15.0f);
    ASSERT_EQ(t.levels, 5);
    ASSERT_EQ(t.width, 16);
    ASSERT_EQ(gpu_tex_level_width(&t, 3), 2);
    ASSERT_EQ(gpu_tex_level_height(&t, 4), 1);
    ASSERT_EQ(gpu_tex_level_height(&t, 9), 1); /* never below one texel */
    ASSERT_EQ(t.level_off[0], 0);
    ASSERT_EQ(t.level_bh[0], 1);
    ASSERT_EQ(t.level_off[1], 1024);
    ASSERT_EQ(t.level_bh[1], 0);
    ASSERT_EQ(t.level_off[2], 1536);
    ASSERT_EQ(t.level_off[3], 2048);
    ASSERT_EQ(t.level_off[4], 2560);
    ASSERT_EQ(t.size, 3072);
    ASSERT_EQ(t.max_level, 4);

    /* texelFetch reads every texel of every level where the reference put it. */
    float c[4];
    for (uint32_t l = 0; l < 5; l++) {
        for (uint32_t y = 0; y < gpu_tex_level_height(&t, l); y++) {
            for (uint32_t x = 0; x < gpu_tex_level_width(&t, l); x++) {
                gpu_tex_fetch(&t, l, (int32_t)x, (int32_t)y, c);
                ASSERT_TRUE(is_texel(c, l, x, y));
            }
        }
    }
    /* a level past the last clamps, a negative one (a huge unsigned) too */
    gpu_tex_fetch(&t, 7, 0, 0, c);
    ASSERT_TRUE(is_level_color(c, 4));
    gpu_tex_fetch(&t, (uint32_t)-1, 0, 0, c);
    ASSERT_TRUE(is_level_color(c, 4));
}

/* 128x128 with 16-GOB blocks, deko3d's usual: the block height halves with
 * each level while half a block still covers it, down to one GOB. */
TEST(mip_chain_with_tall_blocks) {
    uint32_t tic[8], tsc[8];
    make_tic(tic, FMT_RGBA8, 128, 128, 4, 8, 0x100);
    make_tsc(tsc, NEAREST, NEAREST, MIP_NEAREST, 0.0f, 0.0f, 15.0f);
    gpu_texture t;
    ASSERT_TRUE(gpu_tex_init(&t, tic, tsc, map, NULL));
    static const uint8_t bh[8] = {4, 3, 2, 1, 0, 0, 0, 0};
    static const uint64_t off[8] = {0,     65536, 81920, 86016,
                                    87040, 87552, 88064, 88576}; /* 64K, 16K, 4K, 1K, 512 x4 */
    for (uint32_t l = 0; l < 8; l++) {
        ASSERT_EQ(t.level_bh[l], bh[l]);
        ASSERT_EQ(t.level_off[l], off[l]);
    }
    ASSERT_EQ(t.size, 89088);
    /* texels at the far corner of levels 0..3, through the reference swizzle */
    float c[4];
    for (uint32_t l = 0; l < 4; l++) {
        uint32_t w = gpu_tex_level_width(&t, l), h = gpu_tex_level_height(&t, l);
        uint8_t px[4] = {7, 9, 11, 255};
        memcpy(level_mem(&t, l) + ref_offset(4 * (w - 1), h - 1, w * 4, t.level_bh[l]), px, 4);
        gpu_tex_fetch(&t, l, (int32_t)w - 1, (int32_t)h - 1, c);
        ASSERT_TRUE(near(c[0], 7.0f / 255) && near(c[1], 9.0f / 255) && near(c[2], 11.0f / 255));
    }
}

/* A BC1 chain below one block: 8x8, 4x4, 2x2, 1x1 texels are 2x2, 1x1, 1x1 and
 * 1x1 blocks of 8 bytes, each level one GOB. */
TEST(bc1_chain_below_one_block) {
    uint32_t tic[8], tsc[8];
    make_tic(tic, FMT_BC1, 8, 8, 0, 4, 0x100);
    make_tsc(tsc, NEAREST, NEAREST, MIP_NEAREST, 0.0f, 0.0f, 15.0f);
    gpu_texture t;
    memset(g_mem + 0x100, 0, 2048);
    ASSERT_TRUE(gpu_tex_init(&t, tic, tsc, map, NULL));
    ASSERT_EQ(t.levels, 4);
    for (uint32_t l = 0; l < 4; l++) {
        ASSERT_EQ(t.level_off[l], 512 * l);
        ASSERT_EQ(t.level_row_bytes[l], l == 0 ? 16 : 8);
    }
    ASSERT_EQ(t.size, 2048);
    /* level 0: block (1, 1) at the reference's address holds the four-colour block */
    memcpy(g_mem + 0x100 + ref_offset(8, 1, 16, 0), k_color4, 8);
    float c[4];
    gpu_tex_fetch(&t, 0, 5, 4, c); /* texel (1, 0) of that block: blue */
    ASSERT_TRUE(near(c[2], 1.0f) && near(c[0], 0.0f));
    /* level 2: one 2x2 texel block */
    memcpy(g_mem + 0x100 + 1024, k_color4, 8);
    gpu_tex_fetch(&t, 2, 1, 1, c); /* row 1: index 3 */
    ASSERT_TRUE(near(c[0], 1.0f / 3.0f) && near(c[2], 2.0f / 3.0f));
    gpu_tex_fetch(&t, 3, 0, 0, c); /* level 3 is empty memory: black */
    ASSERT_TRUE(near(c[0], 0.0f));
}

TEST(mip_level_from_derivatives) {
    gpu_texture t;
    mip_texture(&t, LINEAR, NEAREST, MIP_NEAREST, 0.0f, 0.0f, 15.0f);
    shader_tex_coord c = {.u = 0.3f, .v = 0.6f, .has_grad = 1};
    float out[4];
    /* One texel per pixel: lambda 0, magnification filter, level 0. */
    c.dudx = 1.0f / 16;
    c.dvdy = 1.0f / 16;
    ASSERT_TRUE(near(gpu_tex_lambda(&t, &c), 0.0f));
    gpu_tex_sample(&t, &c, out);
    ASSERT_TRUE(is_level_color(out, 0));
    /* Four texels per pixel along x: lambda 2, level 2. */
    c.dudx = 4.0f / 16;
    ASSERT_TRUE(near(gpu_tex_lambda(&t, &c), 2.0f));
    gpu_tex_sample(&t, &c, out);
    ASSERT_TRUE(is_level_color(out, 2));
    /* The longer axis decides: y at 8 texels per pixel is level 3. */
    c.dvdy = 8.0f / 16;
    gpu_tex_sample(&t, &c, out);
    ASSERT_TRUE(is_level_color(out, 3));
    /* Lambda 2.4 rounds to level 2, 2.6 to level 3 (nearest mip). */
    c.dvdy = 1.0f / 16;
    c.dudx = powf(2.0f, 2.4f) / 16;
    gpu_tex_sample(&t, &c, out);
    ASSERT_TRUE(is_level_color(out, 2));
    c.dudx = powf(2.0f, 2.6f) / 16;
    gpu_tex_sample(&t, &c, out);
    ASSERT_TRUE(is_level_color(out, 3));
    /* Far beyond the chain: the last level. */
    c.dudx = 64.0f;
    gpu_tex_sample(&t, &c, out);
    ASSERT_TRUE(is_level_color(out, 4));
    /* No derivatives (a lane that ran alone): level 0. */
    c.has_grad = 0;
    gpu_tex_sample(&t, &c, out);
    ASSERT_TRUE(is_level_color(out, 0));
}

TEST(mip_linear_blends_two_levels) {
    gpu_texture t;
    mip_texture(&t, LINEAR, LINEAR, MIP_LINEAR, 0.0f, 0.0f, 15.0f);
    shader_tex_coord c = {.u = 0.5f,
                          .v = 0.5f,
                          .has_grad = 1,
                          .dudx = 2.0f / 16,
                          .dvdy = 1.0f / 16}; /* lambda 1: all of level 1 */
    float out[4];
    gpu_tex_sample(&t, &c, out);
    ASSERT_TRUE(is_level_color(out, 1));
    c.dudx = powf(2.0f, 1.5f) / 16; /* lambda 1.5: half level 1, half level 2 */
    gpu_tex_sample(&t, &c, out);
    ASSERT_TRUE(near(out[0], 51.0f * 1.5f / 255.0f));
    c.dudx = powf(2.0f, 3.75f) / 16; /* lambda 3.75: a quarter level 3, three quarters level 4 */
    gpu_tex_sample(&t, &c, out);
    ASSERT_TRUE(near(out[0], 51.0f * 3.75f / 255.0f));
}

TEST(lod_bias_clamps_and_explicit_modes) {
    gpu_texture t;
    mip_texture(&t, NEAREST, NEAREST, MIP_NEAREST, 1.0f, 0.5f, 2.5f);
    ASSERT_TRUE(near(t.lod_bias, 1.0f));
    ASSERT_TRUE(near(t.lod_min, 0.5f));
    ASSERT_TRUE(near(t.lod_max, 2.5f));
    shader_tex_coord c = {
        .u = 0.1f, .v = 0.1f, .has_grad = 1, .dudx = 1.0f / 16, .dvdy = 1.0f / 16};
    float out[4];
    /* Implicit 0 + bias 1 = 1. */
    ASSERT_TRUE(near(gpu_tex_lambda(&t, &c), 1.0f));
    gpu_tex_sample(&t, &c, out);
    ASSERT_TRUE(is_level_color(out, 1));
    /* 4 + 1 clamps to 2.5: level 2 by the nearest rule (ceil(2.5 + 0.5) - 1). */
    c.dudx = 16.0f / 16;
    ASSERT_TRUE(near(gpu_tex_lambda(&t, &c), 2.5f));
    gpu_tex_sample(&t, &c, out);
    ASSERT_TRUE(is_level_color(out, 2));
    /* .LZ: a base of 0, then the bias and the clamps like any other: 1. */
    c.lod_mode = SHADER_LOD_ZERO;
    ASSERT_TRUE(near(gpu_tex_lambda(&t, &c), 1.0f));
    gpu_tex_sample(&t, &c, out);
    ASSERT_TRUE(is_level_color(out, 1));
    /* .LL: the level given plus the bias, clamped. */
    c.lod_mode = SHADER_LOD_EXPLICIT;
    c.lod = 3.0f;
    gpu_tex_sample(&t, &c, out);
    ASSERT_TRUE(is_level_color(out, 2)); /* 4 clamps to 2.5 */
    c.lod = -0.75f;
    ASSERT_TRUE(near(gpu_tex_lambda(&t, &c), 0.5f)); /* 0.25 clamps up to 0.5 */
    gpu_tex_sample(&t, &c, out);
    ASSERT_TRUE(is_level_color(out, 0)); /* 0.5 stays on the base level */
    /* a NaN derivative (a helper pixel off the edge) counts as no detail */
    c.lod_mode = SHADER_LOD_IMPLICIT;
    c.dudx = NAN;
    ASSERT_TRUE(near(gpu_tex_lambda(&t, &c), 1.0f)); /* 0 + bias, clamped */
    gpu_tex_sample(&t, &c, out);
    ASSERT_TRUE(is_level_color(out, 1));
    /* .LB: implicit 0 plus the instruction's bias plus the sampler's. */
    c.lod_mode = SHADER_LOD_BIAS;
    c.lod = 1.0f;
    c.dudx = 1.0f / 16;
    ASSERT_TRUE(near(gpu_tex_lambda(&t, &c), 2.0f));
    gpu_tex_sample(&t, &c, out);
    ASSERT_TRUE(is_level_color(out, 2));
}

TEST(image_min_lod_clamp) {
    uint32_t tic[8], tsc[8];
    make_tic(tic, FMT_RGBA8, 16, 16, 1, 5, 0x100);
    tic_of(tic)->min_lod_clamp = 2 * 256; /* the image never magnifies below level 2 */
    make_tsc(tsc, NEAREST, NEAREST, MIP_NEAREST, 0.0f, 0.5f, 15.0f);
    gpu_texture t;
    ASSERT_TRUE(gpu_tex_init(&t, tic, tsc, map, NULL));
    ASSERT_TRUE(near(t.lod_min, 2.0f)); /* the larger of the sampler's and the image's */
    for (uint32_t l = 0; l < 5; l++) {
        fill_level(&t, l);
    }
    shader_tex_coord c = {.u = 0.1f, .v = 0.1f, .has_grad = 1, .dudx = 1.0f / 16};
    float out[4];
    gpu_tex_sample(&t, &c, out);
    ASSERT_TRUE(is_level_color(out, 2));
}

TEST(view_range_and_minification_filter) {
    uint32_t tic[8], tsc[8];
    make_tic(tic, FMT_RGBA8, 16, 16, 1, 5, 0x100);
    tic_of(tic)->view_mip_min_level = 1; /* the view sees levels 1..3 */
    tic_of(tic)->view_mip_max_level = 3;
    make_tsc(tsc, LINEAR, NEAREST, MIP_NONE, 0.0f, 0.0f, 15.0f);
    gpu_texture t;
    ASSERT_TRUE(gpu_tex_init(&t, tic, tsc, map, NULL));
    for (uint32_t l = 0; l < 5; l++) {
        fill_level(&t, l);
    }
    ASSERT_EQ(t.min_level, 1);
    ASSERT_EQ(t.max_level, 3);
    float out[4];
    shader_tex_coord c = {.u = 0.0f,
                          .v = 0.0f,
                          .has_grad = 1,
                          .dudx = 1.0f / 8,
                          .dvdy = 1.0f / 8}; /* one texel of level 1 per pixel */
    ASSERT_TRUE(near(gpu_tex_lambda(&t, &c), 0.0f));
    gpu_tex_sample(&t, &c, out);
    ASSERT_TRUE(is_level_color(out, 1)); /* the view's first level */
    c.dudx = 1.0f; /* minified, no mip filter: the base level with the minification filter */
    gpu_tex_sample(&t, &c, out);
    ASSERT_TRUE(is_level_color(out, 1));
    gpu_tex_fetch(&t, 2, 0, 0, out); /* texelFetch level 2 is the view's third */
    ASSERT_TRUE(is_level_color(out, 3));

    /* Filters: level 0 holds two colours; magnified it blends, minified with a
     * nearest minification filter it does not. */
    uint32_t red = 0xFF0000FF, green = 0xFF00FF00;
    memcpy(g_mem + 0x100 + ref_offset(0, 0, 64, 1), &red, 4);   /* texel (0,0) of level 0 */
    memcpy(g_mem + 0x100 + ref_offset(4, 0, 64, 1), &green, 4); /* texel (1,0) */
    tic_of(tic)->view_mip_min_level = 0;
    tic_of(tic)->view_mip_max_level = 4;
    ASSERT_TRUE(gpu_tex_init(&t, tic, tsc, map, NULL));
    c.u = 1.0f / 16; /* between the two texel centres */
    c.v = 0.5f / 16;
    c.dudx = 1.0f / 16;
    c.dvdy = 1.0f / 16;
    gpu_tex_sample(&t, &c, out); /* lambda 0: linear magnification */
    ASSERT_TRUE(near(out[0], 0.5f) && near(out[1], 0.5f));
    c.dudx = 2.0f / 16; /* lambda 1, mip none: level 0 with the nearest filter */
    gpu_tex_sample(&t, &c, out);
    ASSERT_TRUE(near(out[1], 1.0f) && near(out[0], 0.0f));
}

TEST(pitch_linear_has_one_level) {
    uint32_t tic[8], tsc[8];
    make_tic(tic, FMT_RGBA8, 16, 16, 0, 4, 0x100); /* a level count that is ignored */
    tic_of(tic)->hdr_version = HDR_VERSION_PITCH;
    tic[3] = (tic[3] & 0xFFFF0000u) | (64 >> 5); /* pitch 64 bytes in the low half */
    tic_of(tic)->texture_type = TEXTURE_2D_NO_MIPMAP;
    make_tsc(tsc, NEAREST, NEAREST, MIP_NEAREST, 0.0f, 0.0f, 15.0f);
    gpu_texture t;
    ASSERT_TRUE(gpu_tex_init(&t, tic, tsc, map, NULL));
    ASSERT_EQ(t.levels, 1);
    ASSERT_EQ(t.size, 64 * 16);
    uint32_t px = 0xFF0000FF;
    memcpy(g_mem + 0x100 + 64 * 5 + 4 * 7, &px, 4);
    float out[4];
    gpu_tex_fetch(&t, 0, 7, 5, out);
    ASSERT_TRUE(near(out[0], 1.0f) && near(out[1], 0.0f));
    shader_tex_coord c = {.u = 7.5f / 16, .v = 5.5f / 16, .has_grad = 1, .dudx = 8.0f / 16};
    gpu_tex_sample(&t, &c, out); /* minified with one level: still that texel */
    ASSERT_TRUE(near(out[0], 1.0f));
}

/* Signed normalised texels: int8 endpoints, -128 reads as -127, results in -1..1. */
TEST(bc4_bc5_and_rgba8_snorm) {
    uint8_t block[16];
    memset(block, 0, sizeof(block));
    block[0] = 127; /* a0 > a1: six interpolated values */
    block[1] = (uint8_t)-127;
    alpha_indices(block);
    block[8] = (uint8_t)-128; /* reads as -127; a0 <= a1: four values plus -1 and 1 */
    block[9] = 64;
    alpha_indices(block + 8);
    float c[4];
    gpu_bc_texel_snorm(FMT_BC4, block, 0, 0, c); /* index 0: 127/127 */
    ASSERT_TRUE(near(c[0], 1.0f) && near(c[1], 0.0f) && near(c[3], 1.0f));
    gpu_bc_texel_snorm(FMT_BC4, block, 2, 0, c); /* index 2: (6*127 - 127)/7 = 635/7 */
    ASSERT_TRUE(near(c[0], 635.0f / 7.0f / 127.0f));
    gpu_bc_texel_snorm(FMT_BC5, block, 2, 0, c); /* green index 2: (4*-127 + 64)/5 */
    ASSERT_TRUE(near(c[1], (4.0f * -127.0f + 64.0f) / 5.0f / 127.0f));
    gpu_bc_texel_snorm(FMT_BC5, block, 2, 1, c); /* texel 6: green index 6 = -1 */
    ASSERT_TRUE(near(c[1], -1.0f));
    gpu_bc_texel_snorm(FMT_BC5, block, 3, 1, c); /* texel 7: green index 7 = 1 */
    ASSERT_TRUE(near(c[1], 1.0f));

    /* through a descriptor: the component type selects the signed decode */
    uint32_t tic[8], tsc[8];
    make_tic(tic, FMT_BC5, 4, 4, 0, 1, 0x100);
    tic_of(tic)->r_type = tic_of(tic)->g_type = TYPE_SNORM;
    make_tsc(tsc, NEAREST, NEAREST, MIP_NONE, 0.0f, 0.0f, 15.0f);
    gpu_texture t;
    ASSERT_TRUE(gpu_tex_init(&t, tic, tsc, map, NULL));
    ASSERT_EQ(t.snorm, 1);
    memcpy(g_mem + 0x100, block, 16);
    float out[4];
    gpu_tex_fetch(&t, 0, 2, 1, out);
    ASSERT_TRUE(near(out[0], (2.0f * 127.0f - 5.0f * 127.0f) / 7.0f / 127.0f)); /* red index 6 */
    ASSERT_TRUE(near(out[1], -1.0f));
    /* RGBA8 snorm: bytes as int8 over 127 */
    make_tic(tic, FMT_RGBA8, 4, 4, 0, 1, 0x100);
    tic_of(tic)->r_type = tic_of(tic)->g_type = tic_of(tic)->b_type = tic_of(tic)->a_type =
        TYPE_SNORM;
    ASSERT_TRUE(gpu_tex_init(&t, tic, tsc, map, NULL));
    uint8_t px[4] = {(uint8_t)-128, (uint8_t)-64, 0, 127};
    memcpy(g_mem + 0x100 + ref_offset(0, 0, 16, 0), px, 4);
    gpu_tex_fetch(&t, 0, 0, 0, out);
    ASSERT_TRUE(near(out[0], -1.0f) && near(out[1], -64.0f / 127.0f) && near(out[2], 0.0f) &&
                near(out[3], 1.0f));
}

/* The sRGB flag linearises RGB (not alpha) of 8-bit texels and BC1-BC3. */
TEST(srgb_texels_decode_to_linear) {
    uint32_t tic[8], tsc[8];
    make_tic(tic, FMT_RGBA8, 4, 4, 0, 1, 0x100);
    tic_of(tic)->is_srgb = 1;
    make_tsc(tsc, NEAREST, NEAREST, MIP_NONE, 0.0f, 0.0f, 15.0f);
    gpu_texture t;
    ASSERT_TRUE(gpu_tex_init(&t, tic, tsc, map, NULL));
    ASSERT_EQ(t.srgb, 1);
    uint8_t px[4] = {128, 255, 0, 128};
    memcpy(g_mem + 0x100 + ref_offset(0, 0, 16, 0), px, 4);
    float out[4];
    gpu_tex_fetch(&t, 0, 0, 0, out);
    ASSERT_TRUE(near(out[0], 0.2158f) && near(out[1], 1.0f) && near(out[2], 0.0f));
    ASSERT_TRUE(near(out[3], 128.0f / 255.0f));
    make_tic(tic, FMT_BC1, 4, 4, 0, 1, 0x100);
    tic_of(tic)->is_srgb = 1;
    ASSERT_TRUE(gpu_tex_init(&t, tic, tsc, map, NULL));
    memcpy(g_mem + 0x100, k_color4, 8);
    gpu_tex_fetch(&t, 0, 2, 0, out); /* 2/3 red encoded: 0.4019 linear */
    ASSERT_TRUE(near(out[0], 0.4020f) && near(out[2], 0.0908f));
}

TEST(unknown_format_is_rejected) {
    uint32_t tic[8], tsc[8];
    make_tic(tic, 0x7F, 4, 4, 0, 1, 0x100);
    make_tsc(tsc, NEAREST, NEAREST, MIP_NONE, 0.0f, 0.0f, 15.0f);
    gpu_texture t;
    ASSERT_EQ(gpu_tex_init(&t, tic, tsc, map, NULL), 0);
    ASSERT_EQ(t.format, 0x7F);
    ASSERT_EQ(gpu_tex_unit_bytes(FMT_BC3), 16);
    ASSERT_EQ(gpu_tex_unit_bytes(FMT_BC4), 8);
}

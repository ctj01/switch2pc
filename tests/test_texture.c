/*
 * Textures (M4, docs/gpu.md): block-compressed texels decoded against values
 * worked out by hand from the formats' definitions, the mip chain laid out as
 * deko3d does, and the level of detail picking and blending levels the way the
 * OpenGL rules say. The image memory is a buffer this test owns; the TIC's
 * address is its offset into that buffer.
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
enum { SWIZZLE_RGBA = 2 | (3 << 3) | (4 << 6) | (5 << 9) };
enum { NEAREST = 1, LINEAR = 2, MIP_NONE = 1, MIP_NEAREST = 2, MIP_LINEAR = 3 };

static uint8_t g_mem[0x10000] __attribute__((aligned(64)));

static const void *map(void *user, uint64_t va, uint64_t size) {
    (void)user;
    return va + size <= sizeof(g_mem) ? g_mem + va : NULL;
}

static int near(float a, float b) {
    return fabsf(a - b) < 1.0f / 512.0f;
}

/* A 2D block-linear image at `va`: TIC words as deko3d writes them. */
static void make_tic(uint32_t tic[8], uint32_t format, uint32_t width, uint32_t height,
                     uint32_t bh_log2, uint32_t levels, uint64_t va) {
    memset(tic, 0, 32);
    tic[0] = format | (SWIZZLE_RGBA << 19);
    tic[1] = (uint32_t)va;
    tic[2] = (uint32_t)(va >> 32) | (3u << 21); /* block-linear */
    tic[3] = (bh_log2 << 3) | ((levels - 1) << 28);
    tic[4] = (width - 1) | (1u << 23); /* 2D */
    tic[5] = height - 1;
    tic[7] = ((levels - 1) << 4); /* view: every level */
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
 * A 16x16 RGBA8 image, block height 2 GOBs (16 rows), five levels. Each level
 * is a block-linear surface of its own; its block height shrinks while half a
 * block still covers it (deko3d's calcLevelOffset): level 0 keeps 16 rows and
 * is 64 x 16 = 1024 bytes; from level 1 on the block is one GOB, so each is 64
 * x 8 = 512 bytes. Each level is filled with its own colour.
 */
static void fill_level(const gpu_texture *t, uint32_t level, uint32_t rgba) {
    uint64_t size =
        (level + 1 < t->levels ? t->level_off[level + 1] : t->size) - t->level_off[level];
    for (uint64_t off = 0; off < size; off += 4) {
        memcpy(g_mem + t->level_off[level] + off, &rgba, 4);
    }
}

static const uint32_t k_level_rgba[5] = {0xFF0000FF, 0xFF00FF00, 0xFFFF0000, 0xFF00FFFF,
                                         0xFFFFFF00}; /* red green blue yellow cyan (ABGR) */

static void mip_texture(gpu_texture *t, uint32_t mag, uint32_t min, uint32_t mip, float bias,
                        float lod_min, float lod_max) {
    uint32_t tic[8], tsc[8];
    make_tic(tic, FMT_RGBA8, 16, 16, 1, 5, 0x100);
    make_tsc(tsc, mag, min, mip, bias, lod_min, lod_max);
    ASSERT_TRUE(gpu_tex_init(t, tic, tsc, map, NULL));
    for (uint32_t l = 0; l < 5; l++) {
        fill_level(t, l, k_level_rgba[l]);
    }
}

static int is_level_color(const float c[4], uint32_t level) {
    uint32_t rgba = k_level_rgba[level];
    return near(c[0], (rgba & 0xFF) / 255.0f) && near(c[1], ((rgba >> 8) & 0xFF) / 255.0f) &&
           near(c[2], ((rgba >> 16) & 0xFF) / 255.0f) && near(c[3], 1.0f);
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

    /* texelFetch reads the level asked for; a level past the last clamps. */
    float c[4];
    for (uint32_t l = 0; l < 5; l++) {
        gpu_tex_fetch(&t, l, 0, 0, c);
        ASSERT_TRUE(is_level_color(c, l));
    }
    gpu_tex_fetch(&t, 7, 0, 0, c);
    ASSERT_TRUE(is_level_color(c, 4));
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
    c.dudx = powf(2.0f, 1.5f) / 16; /* lambda 1.5: half green, half blue */
    gpu_tex_sample(&t, &c, out);
    ASSERT_TRUE(near(out[0], 0.0f) && near(out[1], 0.5f) && near(out[2], 0.5f));
    c.dudx = powf(2.0f, 3.75f) / 16; /* lambda 3.75: a quarter yellow, three quarters cyan */
    gpu_tex_sample(&t, &c, out);
    ASSERT_TRUE(near(out[0], 0.25f) && near(out[1], 1.0f) && near(out[2], 0.75f));
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
    /* 4 + 1 clamps to 2.5: level 3 by the nearest rule (ceil(3) - 1 = 2)... */
    c.dudx = 16.0f / 16;
    ASSERT_TRUE(near(gpu_tex_lambda(&t, &c), 2.5f));
    gpu_tex_sample(&t, &c, out);
    ASSERT_TRUE(is_level_color(out, 2));
    /* .LZ ignores everything and takes level 0. */
    c.lod_mode = SHADER_LOD_ZERO;
    ASSERT_TRUE(near(gpu_tex_lambda(&t, &c), 0.0f));
    gpu_tex_sample(&t, &c, out);
    ASSERT_TRUE(is_level_color(out, 0));
    /* .LL: the level given, still clamped (0.5 floor). */
    c.lod_mode = SHADER_LOD_EXPLICIT;
    c.lod = 3.0f;
    gpu_tex_sample(&t, &c, out);
    ASSERT_TRUE(is_level_color(out, 2)); /* 3 clamps to 2.5 */
    c.lod = 0.0f;
    ASSERT_TRUE(near(gpu_tex_lambda(&t, &c), 0.5f));
    gpu_tex_sample(&t, &c, out);
    ASSERT_TRUE(is_level_color(out, 0)); /* 0.5 stays on the base level */
    /* .LB: implicit 0 plus the instruction's bias plus the sampler's. */
    c.lod_mode = SHADER_LOD_BIAS;
    c.lod = 1.0f;
    c.dudx = 1.0f / 16;
    ASSERT_TRUE(near(gpu_tex_lambda(&t, &c), 2.0f));
    gpu_tex_sample(&t, &c, out);
    ASSERT_TRUE(is_level_color(out, 2));
}

TEST(view_range_and_minification_filter) {
    uint32_t tic[8], tsc[8];
    make_tic(tic, FMT_RGBA8, 16, 16, 1, 5, 0x100);
    tic[7] = 1 | (3 << 4); /* the view sees levels 1..3 */
    make_tsc(tsc, LINEAR, NEAREST, MIP_NONE, 0.0f, 0.0f, 15.0f);
    gpu_texture t;
    ASSERT_TRUE(gpu_tex_init(&t, tic, tsc, map, NULL));
    for (uint32_t l = 0; l < 5; l++) {
        fill_level(&t, l, k_level_rgba[l]);
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
    memcpy(g_mem + 0x100, &red, 4);       /* texel (0,0) of level 0 */
    memcpy(g_mem + 0x100 + 4, &green, 4); /* texel (1,0) */
    tic[7] = 0 | (4 << 4);
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
    memset(tic, 0, 32);
    tic[0] = FMT_RGBA8 | (SWIZZLE_RGBA << 19);
    tic[1] = 0x100;
    tic[2] = 2u << 21;               /* pitch */
    tic[3] = (64 >> 5) | (3u << 28); /* pitch 64 bytes; a level count that is ignored */
    tic[4] = 15 | (7u << 23);        /* 16 wide, 2DNoMipmap */
    tic[5] = 15;
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

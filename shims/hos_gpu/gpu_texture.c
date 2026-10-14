/* Texture descriptors, mip chains, block compression and sampling; see gpu_texture.h. */

#include "gpu_texture.h"

#include <math.h>
#include <pthread.h>
#include <string.h>

#include "gpu_draw.h"
#include "swizzle.h"

enum { FMT_BC1 = 0x24, FMT_BC2 = 0x25, FMT_BC3 = 0x26, FMT_BC4 = 0x27, FMT_BC5 = 0x28 };
enum { FILTER_NEAREST = 1, FILTER_LINEAR = 2 };
enum { MIP_NONE = 1, MIP_NEAREST = 2, MIP_LINEAR = 3 };

uint32_t gpu_tex_unit_bytes(uint32_t format) {
    switch (format) {
    case 0x01:
        return 16; /* R32G32B32A32 */
    case 0x03:
        return 8; /* R16G16B16A16 (read as half floats) */
    case 0x04:
        return 8; /* R32G32 */
    case 0x08:
        return 4; /* A8B8G8R8 */
    case 0x0F:
        return 4; /* R32 */
    case 0x15:
        return 2; /* B5G6R5 */
    case 0x18:
        return 2; /* G8R8 */
    case 0x1D:
        return 1; /* R8 */
    case FMT_BC1:
    case FMT_BC4:
        return 8; /* per 4x4 block */
    case FMT_BC2:
    case FMT_BC3:
    case FMT_BC5:
        return 16;
    default:
        return 0;
    }
}

static uint32_t level_dim(uint32_t d, uint32_t level) {
    d >>= level;
    return d ? d : 1;
}

static uint32_t units(uint32_t texels, uint32_t block) {
    return (texels + block - 1) / block;
}

uint32_t gpu_tex_level_width(const gpu_texture *t, uint32_t level) {
    return level_dim(t->width, level);
}

uint32_t gpu_tex_level_height(const gpu_texture *t, uint32_t level) {
    return level_dim(t->height, level);
}

static uint32_t align64(uint32_t bytes) {
    return (bytes + SWIZZLE_GOB_WIDTH - 1) & ~(uint32_t)(SWIZZLE_GOB_WIDTH - 1);
}

/* A level's block height is the image's, halved while half a block still covers
 * the level (deko3d's adjustTileSize, which is what the hardware expects). */
static uint8_t level_block_height(uint8_t bh, uint32_t rows) {
    if (!bh) {
        return 0;
    }
    uint32_t x = (uint32_t)SWIZZLE_GOB_HEIGHT << (bh - 1);
    if (x >= rows) {
        while (--bh) {
            x >>= 1;
            if (x < rows) {
                break;
            }
        }
    }
    return bh;
}

static uint64_t bl_size(uint32_t row_bytes, uint32_t rows, uint8_t bh) {
    uint32_t block_rows = (uint32_t)SWIZZLE_GOB_HEIGHT << bh;
    return (uint64_t)align64(row_bytes) *
           (((uint64_t)rows + block_rows - 1) / block_rows * block_rows);
}

static float fixed_s13(uint32_t v) { /* 13-bit two's complement, 8 fraction bits */
    int32_t s = (int32_t)(v << 19) >> 19;
    return (float)s / 256.0f;
}

int gpu_tex_init(gpu_texture *t, const uint32_t tic[8], const uint32_t tsc[8], gpu_tex_map_fn map,
                 void *user) {
    memset(t, 0, sizeof(*t));
    t->format = tic[0] & 0x7F;
    t->snorm = ((tic[0] >> 7) & 7) == 1; /* R's component type speaks for all */
    t->srgb = (tic[4] >> 22) & 1;
    t->swizzle = (tic[0] >> 19) & 0xFFF;
    t->unit_bytes = gpu_tex_unit_bytes(t->format);
    t->block = t->format >= FMT_BC1 && t->format <= FMT_BC5 ? 4 : 1;
    uint32_t layout = (tic[2] >> 21) & 7;
    t->pitch_layout = layout == 2;
    t->width = (tic[4] & 0xFFFF) + 1;
    t->height = (tic[5] & 0xFFFF) + 1;
    uint32_t type = (tic[4] >> 23) & 0xF;
    uint8_t bh0 = (tic[3] >> 3) & 7;
    t->levels = t->pitch_layout || type == 7 ? 1 : ((tic[3] >> 28) & 0xF) + 1;
    t->min_level = tic[7] & 0xF;
    t->max_level = (tic[7] >> 4) & 0xF;
    if (t->max_level >= t->levels) {
        t->max_level = t->levels - 1;
    }
    if (t->min_level > t->max_level) {
        t->min_level = t->max_level;
    }
    t->wrap_u = tsc[0] & 7;
    t->wrap_v = (tsc[0] >> 3) & 7;
    t->mag_filter = tsc[1] & 3;
    t->min_filter = (tsc[1] >> 4) & 3;
    t->mip_filter = (tsc[1] >> 6) & 3;
    t->lod_bias = fixed_s13((tsc[1] >> 12) & 0x1FFF) + fixed_s13((tic[6] >> 6) & 0x1FFF);
    t->lod_min = (float)(tsc[2] & 0xFFF) / 256.0f;
    float image_min = (float)((tic[7] >> 12) & 0xFFF) / 256.0f; /* TIC min lod clamp */
    t->lod_min = image_min > t->lod_min ? image_min : t->lod_min;
    t->lod_max = (float)((tsc[2] >> 12) & 0xFFF) / 256.0f;
    memcpy(t->border, tsc + 4, 16);
    if (!t->unit_bytes || (layout != 2 && layout != 3)) {
        return 0;
    }
    uint64_t off = 0;
    for (uint32_t l = 0; l < t->levels; l++) {
        uint32_t uw = units(level_dim(t->width, l), t->block);
        uint32_t uh = units(level_dim(t->height, l), t->block);
        t->level_off[l] = off;
        if (t->pitch_layout) {
            t->level_row_bytes[l] = (tic[3] & 0xFFFF) << 5;
            t->level_bh[l] = 0;
            off += (uint64_t)t->level_row_bytes[l] * uh;
        } else {
            t->level_row_bytes[l] = uw * t->unit_bytes;
            t->level_bh[l] = level_block_height(bh0, uh);
            off += bl_size(t->level_row_bytes[l], uh, t->level_bh[l]);
        }
    }
    t->size = off;
    uint64_t va = ((uint64_t)(tic[2] & 0xFFFF) << 32) | tic[1];
    t->base = map(user, va, t->size);
    t->valid = t->base != NULL;
    return t->valid;
}

/* The sRGB transfer function, encoded 0..1 to linear; the 8-bit formats go through
 * a table (powf per texel and channel was a third of an sRGB scene's time in
 * gpu_engines.c), the block formats' interpolated values through the formula. */
static float srgb_to_linear(float v) {
    return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
}

static float g_srgb_dec[256];
static pthread_once_t g_srgb_once = PTHREAD_ONCE_INIT;

static void srgb_init(void) {
    for (int k = 0; k < 256; k++) {
        g_srgb_dec[k] = srgb_to_linear((float)k / 255.0f);
    }
}

/* ---- block compression ------------------------------------------------------------- */

static void rgb565_to_float(uint16_t v, float c[3]) {
    uint32_t r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
    c[0] = (float)((r << 3) | (r >> 2)) / 255.0f; /* bit replication to 8 bits */
    c[1] = (float)((g << 2) | (g >> 4)) / 255.0f;
    c[2] = (float)((b << 3) | (b >> 2)) / 255.0f;
}

/* The BC1 colour block: two RGB565 endpoints, 2-bit indices (row-major, LSB
 * first). On its own, c0 <= c1 selects three colours and transparent black;
 * inside BC2 and BC3 the block is always four colours. */
static void bc1_colors(const uint8_t *b, uint32_t tx, uint32_t ty, int four_only, float c[4]) {
    uint16_t c0 = (uint16_t)(b[0] | b[1] << 8), c1 = (uint16_t)(b[2] | b[3] << 8);
    uint32_t idx = (b[4 + ty] >> (2 * tx)) & 3;
    float e0[3], e1[3];
    rgb565_to_float(c0, e0);
    rgb565_to_float(c1, e1);
    int four = four_only || c0 > c1;
    c[3] = 1.0f;
    for (int k = 0; k < 3; k++) {
        float v;
        if (idx == 0) {
            v = e0[k];
        } else if (idx == 1) {
            v = e1[k];
        } else if (four) {
            v = idx == 2 ? (2.0f * e0[k] + e1[k]) / 3.0f : (e0[k] + 2.0f * e1[k]) / 3.0f;
        } else {
            v = idx == 2 ? (e0[k] + e1[k]) / 2.0f : 0.0f;
        }
        c[k] = v;
    }
    if (!four && idx == 3) {
        c[3] = 0.0f;
    }
}

/* The BC3/BC4/BC5 8-byte channel block: two 8-bit endpoints and 3-bit indices
 * (LSB first); a0 > a1 interpolates six values, otherwise four plus 0 and 1. */
static uint32_t bc3_index(const uint8_t *b, uint32_t tx, uint32_t ty) {
    uint64_t bits = 0;
    for (int k = 5; k >= 0; k--) {
        bits = (bits << 8) | b[2 + k];
    }
    return (uint32_t)(bits >> (3 * (ty * 4 + tx))) & 7;
}

/* The eight-value scheme shared by BC3's alpha, BC4 and BC5: endpoints a0 and a1,
 * six interpolated values between them when a0 > a1, else four plus `lo` and `hi`. */
static float bc3_value(int32_t a0, int32_t a1, uint32_t idx, float lo, float hi) {
    if (idx == 0) {
        return (float)a0;
    }
    if (idx == 1) {
        return (float)a1;
    }
    if (a0 > a1) {
        return (float)((8 - (int32_t)idx) * a0 + ((int32_t)idx - 1) * a1) / 7.0f;
    }
    if (idx == 6) {
        return lo;
    }
    if (idx == 7) {
        return hi;
    }
    return (float)((6 - (int32_t)idx) * a0 + ((int32_t)idx - 1) * a1) / 5.0f;
}

static float bc3_channel(const uint8_t *b, uint32_t tx, uint32_t ty) {
    return bc3_value(b[0], b[1], bc3_index(b, tx, ty), 0.0f, 255.0f) / 255.0f;
}

static float bc3_channel_snorm(const uint8_t *b, uint32_t tx, uint32_t ty) {
    int32_t a0 = (int8_t)b[0], a1 = (int8_t)b[1];
    a0 = a0 < -127 ? -127 : a0;
    a1 = a1 < -127 ? -127 : a1;
    return bc3_value(a0, a1, bc3_index(b, tx, ty), -127.0f, 127.0f) / 127.0f;
}

void gpu_bc_texel_snorm(uint32_t format, const uint8_t *b, uint32_t tx, uint32_t ty, float c[4]) {
    c[0] = bc3_channel_snorm(b, tx, ty);
    c[1] = format == FMT_BC5 ? bc3_channel_snorm(b + 8, tx, ty) : 0.0f;
    c[2] = 0.0f;
    c[3] = 1.0f;
}

void gpu_bc_texel(uint32_t format, const uint8_t *b, uint32_t tx, uint32_t ty, float c[4]) {
    switch (format) {
    case FMT_BC1:
        bc1_colors(b, tx, ty, 0, c);
        break;
    case FMT_BC2: /* 4-bit alpha per texel, then the colour block */
        bc1_colors(b + 8, tx, ty, 1, c);
        c[3] = (float)((b[2 * ty + tx / 2] >> (4 * (tx & 1))) & 0xF) / 15.0f;
        break;
    case FMT_BC3:
        bc1_colors(b + 8, tx, ty, 1, c);
        c[3] = bc3_channel(b, tx, ty);
        break;
    case FMT_BC4:
        c[0] = bc3_channel(b, tx, ty);
        c[1] = c[2] = 0.0f;
        c[3] = 1.0f;
        break;
    case FMT_BC5:
        c[0] = bc3_channel(b, tx, ty);
        c[1] = bc3_channel(b + 8, tx, ty);
        c[2] = 0.0f;
        c[3] = 1.0f;
        break;
    default:
        c[0] = c[1] = c[2] = 0.0f;
        c[3] = 1.0f;
        break;
    }
}

/* ---- fetching ----------------------------------------------------------------------- */

/* Texel coordinate after the wrap mode; -1 selects the border colour. */
static int32_t tex_wrap(int32_t i, int32_t n, uint32_t mode) {
    switch (mode) {
    case 0: /* repeat */
        i %= n;
        return i < 0 ? i + n : i;
    case 1: { /* mirrored repeat */
        int32_t p = 2 * n, m = i % p;
        m = m < 0 ? m + p : m;
        return m < n ? m : p - 1 - m;
    }
    case 3: /* border */
    case 6:
        return i < 0 || i >= n ? -1 : i;
    default: /* clamp to edge and the clamp variants */
        return i < 0 ? 0 : i >= n ? n - 1 : i;
    }
}

static void fetch_level(const gpu_texture *t, uint32_t level, int32_t ix, int32_t iy,
                        float out[4]) {
    ix = tex_wrap(ix, (int32_t)level_dim(t->width, level), t->wrap_u);
    iy = tex_wrap(iy, (int32_t)level_dim(t->height, level), t->wrap_v);
    if (ix < 0 || iy < 0) {
        memcpy(out, t->border, 16);
        return;
    }
    uint32_t ux = (uint32_t)ix / t->block, uy = (uint32_t)iy / t->block;
    uint32_t row_bytes = t->level_row_bytes[level];
    uint64_t off = t->level_off[level] +
                   (t->pitch_layout ? (uint64_t)uy * row_bytes + (uint64_t)ux * t->unit_bytes
                                    : swizzle_offset(ux * t->unit_bytes, uy, align64(row_bytes),
                                                     1u << t->level_bh[level]));
    float c[4] = {0, 0, 0, 1};
    if (off + t->unit_bytes <= t->size) {
        const uint8_t *p = t->base + off;
        /* 8-bit unorm, or snorm: -128 reads as -127 */
        const float scale = t->snorm ? 1.0f / 127.0f : 1.0f / 255.0f;
        int srgb8 = t->srgb && !t->snorm; /* RGB bytes through the table, below */
#define BYTE(v)                                                                                    \
    (t->snorm ? ((int8_t)(v) < -127 ? -127.0f : (float)(int8_t)(v)) : (float)(v)) * scale
        switch (t->format) {
        case 0x08:
            for (int k = 0; k < 4; k++) {
                c[k] = BYTE(p[k]);
            }
            break;
        case 0x15: {
            uint16_t v = (uint16_t)(p[0] | p[1] << 8);
            c[0] = (v & 0x1F) / 31.0f; /* B5G6R5: R in the low bits */
            c[1] = ((v >> 5) & 0x3F) / 63.0f;
            c[2] = ((v >> 11) & 0x1F) / 31.0f;
            break;
        }
        case 0x18:
            c[0] = BYTE(p[0]);
            c[1] = BYTE(p[1]);
            break;
        case 0x1D:
            c[0] = BYTE(p[0]);
            break;
        case 0x0F:
            memcpy(&c[0], p, 4);
            break;
        case 0x04:
            memcpy(c, p, 8);
            break;
        case 0x01:
            memcpy(c, p, 16);
            break;
        case 0x03:
            for (int k = 0; k < 4; k++) {
                c[k] = gpu_half_to_float((uint16_t)(p[2 * k] | p[2 * k + 1] << 8));
            }
            break;
        default:
            if (t->block == 4 && t->snorm && t->format >= FMT_BC4) {
                gpu_bc_texel_snorm(t->format, p, (uint32_t)ix & 3, (uint32_t)iy & 3, c);
            } else if (t->block == 4) {
                gpu_bc_texel(t->format, p, (uint32_t)ix & 3, (uint32_t)iy & 3, c);
            }
            break;
        }
#undef BYTE
        if (srgb8 && (t->format == 0x08 || t->format == 0x18 || t->format == 0x1D)) {
            pthread_once(&g_srgb_once, srgb_init);
            int n = t->format == 0x08 ? 3 : t->format == 0x18 ? 2 : 1; /* alpha stays linear */
            for (int k = 0; k < n; k++) {
                c[k] = g_srgb_dec[p[k]];
            }
        } else if (t->srgb && t->format >= FMT_BC1 && t->format <= FMT_BC3) {
            for (int k = 0; k < 3; k++) {
                c[k] = srgb_to_linear(c[k]);
            }
        }
    }
    /* Swizzle: per destination component, 0 zero, 2-5 R G B A, 6/7 one. */
    for (int k = 0; k < 4; k++) {
        uint32_t src = (t->swizzle >> (3 * k)) & 7;
        out[k] = src >= 2 && src <= 5 ? c[src - 2] : src >= 6 ? 1.0f : 0.0f;
    }
}

void gpu_tex_fetch(const gpu_texture *t, uint32_t level, int32_t x, int32_t y, float out[4]) {
    /* compared before adding: a negative level in the register is a huge one */
    uint32_t l = level > t->max_level - t->min_level ? t->max_level : t->min_level + level;
    fetch_level(t, l, x, y, out);
}

/* ---- sampling ----------------------------------------------------------------------- */

static void sample_level(const gpu_texture *t, uint32_t level, float u, float v, int linear,
                         float out[4]) {
    float fx = u * (float)level_dim(t->width, level), fy = v * (float)level_dim(t->height, level);
    if (!isfinite(fx) || !isfinite(fy)) {
        fx = fy = 0.0f; /* a helper pixel's extrapolated coordinate; floorf's cast would be UB */
    }
    if (!linear) {
        fetch_level(t, level, (int32_t)floorf(fx), (int32_t)floorf(fy), out);
        return;
    }
    fx -= 0.5f;
    fy -= 0.5f;
    int32_t x0 = (int32_t)floorf(fx), y0 = (int32_t)floorf(fy);
    float ax = fx - (float)x0, ay = fy - (float)y0;
    float t00[4], t10[4], t01[4], t11[4];
    fetch_level(t, level, x0, y0, t00);
    fetch_level(t, level, x0 + 1, y0, t10);
    fetch_level(t, level, x0, y0 + 1, t01);
    fetch_level(t, level, x0 + 1, y0 + 1, t11);
    for (int k = 0; k < 4; k++) {
        float top = t00[k] + (t10[k] - t00[k]) * ax, bottom = t01[k] + (t11[k] - t01[k]) * ax;
        out[k] = top + (bottom - top) * ay;
    }
}

float gpu_tex_lambda(const gpu_texture *t, const shader_tex_coord *c) {
    /* OpenGL 4.6 8.14.1: the base from the instruction, the biases, then the clamps. */
    float lambda = 0.0f;
    if (c->lod_mode == SHADER_LOD_EXPLICIT) {
        lambda = c->lod;
    } else if (c->lod_mode != SHADER_LOD_ZERO && c->has_grad) {
        /* The footprint's longer axis in texels of the view's first level. */
        float w = (float)level_dim(t->width, t->min_level);
        float h = (float)level_dim(t->height, t->min_level);
        float dx = hypotf(c->dudx * w, c->dvdx * h), dy = hypotf(c->dudy * w, c->dvdy * h);
        float rho = dx > dy ? dx : dy;
        lambda = rho > 0.0f ? log2f(rho) : -16.0f;
    }
    if (c->lod_mode == SHADER_LOD_BIAS) {
        lambda += c->lod;
    }
    lambda += t->lod_bias;
    if (lambda != lambda) {
        lambda = 0.0f; /* a helper pixel's extrapolated coordinates can be NaN */
    }
    if (lambda < t->lod_min) {
        lambda = t->lod_min;
    }
    if (lambda > t->lod_max) {
        lambda = t->lod_max;
    }
    return lambda;
}

void gpu_tex_sample(const gpu_texture *t, const shader_tex_coord *c, float out[4]) {
    uint32_t base = t->min_level, last = t->max_level;
    if (base == last && t->min_filter == t->mag_filter) {
        /* One level and one filter: the lod would change nothing. */
        sample_level(t, base, c->u, c->v, t->mag_filter == FILTER_LINEAR, out);
        return;
    }
    float lambda = gpu_tex_lambda(t, c);
    if (lambda <= 0.0f) {
        sample_level(t, base, c->u, c->v, t->mag_filter == FILTER_LINEAR, out);
        return;
    }
    int linear = t->min_filter == FILTER_LINEAR;
    if (t->mip_filter == MIP_NEAREST) {
        /* OpenGL's rule: lambda <= 1/2 stays on the base level. */
        uint32_t d = lambda <= 0.5f ? 0 : (uint32_t)ceilf(lambda + 0.5f) - 1;
        uint32_t level = base + d > last ? last : base + d;
        sample_level(t, level, c->u, c->v, linear, out);
    } else if (t->mip_filter == MIP_LINEAR) {
        float fl = floorf(lambda);
        uint32_t l0 = base + (uint32_t)fl > last ? last : base + (uint32_t)fl;
        uint32_t l1 = l0 + 1 > last ? last : l0 + 1;
        sample_level(t, l0, c->u, c->v, linear, out);
        if (l1 != l0) {
            float frac = lambda - fl, hi[4];
            sample_level(t, l1, c->u, c->v, linear, hi);
            for (int k = 0; k < 4; k++) {
                out[k] += (hi[k] - out[k]) * frac;
            }
        }
    } else { /* no mipmapping: the base level with the minification filter */
        sample_level(t, base, c->u, c->v, linear, out);
    }
}

/* ---- half precision, shared with the engines ---------------------------------------- */

uint16_t gpu_float_to_half(float v) {
    uint32_t x;
    memcpy(&x, &v, 4);
    uint32_t sign = (x >> 16) & 0x8000, e = (x >> 23) & 0xFF, m = x & 0x7FFFFF;
    if (e == 0xFF) {
        return (uint16_t)(sign | 0x7C00 | (m ? 0x200 : 0)); /* inf, nan */
    }
    int32_t he = (int32_t)e - 127 + 15;
    if (he >= 0x1F) {
        return (uint16_t)(sign | 0x7C00);
    }
    if (he <= 0) { /* subnormal or zero */
        if (he < -10) {
            return (uint16_t)sign;
        }
        m |= 0x800000;
        uint32_t shift = (uint32_t)(14 - he), hm = m >> shift;
        hm += (m >> (shift - 1)) & 1; /* round half up */
        return (uint16_t)(sign | hm);
    }
    uint32_t h = sign | ((uint32_t)he << 10) | (m >> 13);
    h += (m >> 12) & 1; /* round half up; a carry into the exponent is still right */
    return (uint16_t)h;
}

float gpu_half_to_float(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000) << 16, e = (h >> 10) & 0x1F, m = h & 0x3FF, x;
    if (e == 0) {
        float v = (float)m / 16777216.0f; /* m * 2^-24 */
        return sign ? -v : v;
    }
    x = sign | (e == 0x1F ? 0x7F800000 | (m << 13) : ((e + 112) << 23) | (m << 13));
    float v;
    memcpy(&v, &x, 4);
    return v;
}

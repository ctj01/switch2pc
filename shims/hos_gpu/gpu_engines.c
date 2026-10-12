/*
 * GPU engines executed on the CPU (M1); see gpu_engines.h. Method numbers
 * are word offsets (byte offset / 4) from the public NVIDIA class headers
 * (clb197, clb0b5, cla140) as published by nouveau and envytools.
 */

#include "gpu_engines.h"

#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common/hos_log.h"
#include "gpu_draw.h"
#include "gpu_internal.h"
#include "gpu_mme.h"
#include "swizzle.h"

#define MOD "engine"

#define METHODS 0x2000 /* 13-bit method field */

enum engine_kind { ENG_NONE = -1, ENG_3D, ENG_COMPUTE, ENG_I2M, ENG_DMA, ENG_2D, ENG_COUNT };

/* Inline-to-memory (shared by 3D, compute and 0xA140 at the same offsets). */
enum {
    I2M_LINE_LENGTH_IN = 0x60,
    I2M_LINE_COUNT = 0x61,
    I2M_OFFSET_OUT_UPPER = 0x62,
    I2M_OFFSET_OUT = 0x63,
    I2M_PITCH_OUT = 0x64,
    I2M_DST_BLOCK_SIZE = 0x65,
    I2M_DST_WIDTH = 0x66,
    I2M_DST_HEIGHT = 0x67,
    I2M_DST_ORIGIN_X = 0x6A,
    I2M_DST_ORIGIN_Y = 0x6B,
    I2M_LAUNCH_DMA = 0x6C,
    I2M_LOAD_INLINE_DATA = 0x6D,
};

/* 3D. */
enum {
    M3D_RT_BASE = 0x200, /* SET_COLOR_TARGET_*(i) at 0x200 + 16 * i */
    M3D_RT_STRIDE = 0x10,
    M3D_RT_ADDR_HI = 0,
    M3D_RT_ADDR_LO = 1,
    M3D_RT_WIDTH = 2,
    M3D_RT_HEIGHT = 3,
    M3D_RT_FORMAT = 4,
    M3D_RT_MEMORY = 5,             /* block width/height/depth log2, [12] pitch layout */
    M3D_COLOR_CLEAR_VALUE = 0x360, /* R, G, B, A as floats */
    M3D_SCISSOR_BASE = 0x380,      /* (i): ENABLE, HORIZONTAL, VERTICAL at 0x380 + 4i */
    M3D_CLEAR_CONTROL = 0x43E,     /* [8] use scissor */
    M3D_CT_SELECT = 0x487,         /* [3:0] count, [6:4] target 0, ... */
    M3D_CLEAR_SURFACE = 0x674,     /* [5:2] RGBA mask, [9:6] MRT, [25:10] layer */
    M3D_REPORT_SEM_A = 0x6C0,
    M3D_REPORT_SEM_B = 0x6C1,
    M3D_REPORT_SEM_C = 0x6C2,
    M3D_REPORT_SEM_D = 0x6C3,
};

/* DMA copy. */
enum {
    DMA_LAUNCH = 0xC0,
    DMA_SEM_ADDR_HI = 0x90,
    DMA_SEM_ADDR_LO = 0x91,
    DMA_SEM_PAYLOAD = 0x92,
    DMA_OFFSET_IN_HI = 0x100,
    DMA_OFFSET_IN_LO = 0x101,
    DMA_OFFSET_OUT_HI = 0x102,
    DMA_OFFSET_OUT_LO = 0x103,
    DMA_PITCH_IN = 0x104,
    DMA_PITCH_OUT = 0x105,
    DMA_LINE_LENGTH_IN = 0x106,
    DMA_LINE_COUNT = 0x107,
    DMA_REMAP_CONST_A = 0x1C0,
    DMA_REMAP_CONST_B = 0x1C1,
    DMA_REMAP_COMPONENTS = 0x1C2,
    DMA_DST_BLOCK_SIZE = 0x1C3,
    DMA_DST_WIDTH = 0x1C4,
    DMA_DST_HEIGHT = 0x1C5,
    DMA_DST_ORIGIN = 0x1C8, /* x[15:0] bytes, y[31:16] */
    DMA_SRC_BLOCK_SIZE = 0x1CA,
    DMA_SRC_WIDTH = 0x1CB,
    DMA_SRC_HEIGHT = 0x1CC,
    DMA_SRC_ORIGIN = 0x1CF,
};

typedef struct i2m_state {
    int active;
    uint32_t launch;
    uint64_t written; /* bytes streamed so far */
    uint64_t total;
} i2m_state;

/* 3D macro upload and calls (gpu_mme.c). */
enum {
    MME_CODE_POINTER = 0x45,
    MME_CODE_DATA = 0x46,
    MME_START_POINTER = 0x47,
    MME_START_DATA = 0x48,
    MME_CALL_BASE = 0xE00, /* 0xE00 + 2n: call macro n; 0xE01 + 2n: more parameters */
    MME_CODE_WORDS = 0x2000,
    MME_MACROS = 0x80,
    MME_MAX_PARAMS = 0x1000,
};

typedef struct mme_call {
    int pending;
    uint32_t macro;
    uint32_t num_params;
    uint32_t params[MME_MAX_PARAMS];
} mme_call;

struct gpu_engines {
    uint32_t regs[ENG_COUNT][METHODS];
    int subch[8]; /* engine kind bound to each subchannel */
    i2m_state i2m[ENG_COUNT];
    gpu_engine_stats stats;
    uint32_t warned_format;
    uint32_t mme_code[MME_CODE_WORDS];
    uint32_t mme_start[MME_MACROS];
    mme_call call;
    uint64_t cb_addr[GPU_STAGES][GPU_CBUFS]; /* constant buffers bound per stage */
    uint32_t cb_size[GPU_STAGES][GPU_CBUFS];
    uint32_t instance;
    int indexed; /* the last count written was an index count */
    gpu_draw_stats draw;
};

static uint64_t addr64(const uint32_t *r, uint32_t hi, uint32_t lo) {
    return ((uint64_t)(r[hi] & 0xFF) << 32) | r[lo];
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int gpu_semaphore_release(uint64_t va, uint32_t payload, int four_words) {
    uint32_t *p = nv_gpu_va_to_host(va, four_words ? 16 : 4);
    if (!p) {
        HOS_LOGW(MOD, "semaphore release to unmapped gpu va %#" PRIx64, va);
        return -1;
    }
    p[0] = payload;
    if (four_words) {
        uint64_t ts = now_ns();
        p[1] = 0;
        memcpy(p + 2, &ts, 8);
    }
    return 0;
}

/* ---- block-linear helpers ---------------------------------------------------------- */

/* Byte offset of (x bytes, y) in a block-linear surface `width_bytes` wide. */
uint64_t gpu_bl_offset(uint32_t x, uint32_t y, uint32_t width_bytes, uint32_t block_h_log2) {
    uint32_t stride = (width_bytes + SWIZZLE_GOB_WIDTH - 1) & ~(uint32_t)(SWIZZLE_GOB_WIDTH - 1);
    return swizzle_offset(x, y, stride, 1u << block_h_log2);
}

/* Size in bytes of a block-linear surface (one layer). */
uint64_t gpu_bl_size(uint32_t width_bytes, uint32_t height, uint32_t block_h_log2) {
    uint32_t stride = (width_bytes + SWIZZLE_GOB_WIDTH - 1) & ~(uint32_t)(SWIZZLE_GOB_WIDTH - 1);
    uint32_t rows = SWIZZLE_GOB_HEIGHT << block_h_log2;
    return (uint64_t)stride * (((uint64_t)height + rows - 1) / rows * rows);
}

/* ---- 3D: clears ----------------------------------------------------------------------- */

static uint8_t unorm8(float v) {
    if (!(v > 0.0f)) {
        return 0;
    }
    if (v >= 1.0f) {
        return 255;
    }
    return (uint8_t)(v * 255.0f + 0.5f);
}

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

void gpu_fill(uint8_t *base, uint64_t size, const uint8_t *px, uint32_t bpp) {
    if (!bpp || size < bpp) {
        return;
    }
    uint64_t total = size - size % bpp, done = bpp;
    memcpy(base, px, bpp);
    while (done < total) { /* doubling copies: a few large memcpy calls */
        uint64_t n = done < total - done ? done : total - done;
        memcpy(base + done, base, n);
        done += n;
    }
}

static float linear_to_srgb(float v) {
    if (!(v > 0.0f)) {
        return 0.0f;
    }
    if (v >= 1.0f) {
        return 1.0f;
    }
    return v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
}

static float srgb_to_linear(float v) {
    return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
}

/*
 * sRGB through tables, with the same results as the formulas: decoding is a
 * function of the byte, and encoding finds the byte among thresholds built
 * from linear_to_srgb, so it returns unorm8(linear_to_srgb(v)) for every v.
 * powf per channel per pixel was a third of an sRGB scene's time.
 */
static float g_srgb_dec[256];
static float g_srgb_thr[256]; /* [k]: smallest v in (0, 1) that encodes to k or more */
/* By the top bits of v (exponent and 8 mantissa bits): the byte of the smallest
 * float in that bucket. A bucket spans less than one byte step, so the answer is
 * it or the next. */
static uint8_t g_srgb_start[0x3F800000 >> 15];
static pthread_once_t g_srgb_once = PTHREAD_ONCE_INIT;

static float float_of(uint32_t b) {
    float v;
    memcpy(&v, &b, 4);
    return v;
}

static void srgb_init(void) {
    for (int k = 0; k < 256; k++) {
        g_srgb_dec[k] = srgb_to_linear((float)k / 255.0f);
    }
    g_srgb_thr[0] = 0.0f;
    for (int k = 1; k < 256; k++) {
        /* positive floats order like their bit patterns: bisect on the bits of (0, 1] */
        uint32_t lo = 1, hi = 0x3F800000;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            if (unorm8(linear_to_srgb(float_of(mid))) >= k) {
                hi = mid;
            } else {
                lo = mid + 1;
            }
        }
        g_srgb_thr[k] = float_of(lo);
    }
    uint32_t k = 0;
    for (uint32_t b = 0; b < (0x3F800000 >> 15); b++) {
        float v = float_of(b << 15);
        while (k < 255 && g_srgb_thr[k + 1] <= v) {
            k++;
        }
        g_srgb_start[b] = (uint8_t)k;
    }
}

static uint8_t srgb8(float v) {
    if (!(v > 0.0f)) {
        return 0;
    }
    if (v >= 1.0f) {
        return 255;
    }
    uint32_t b;
    memcpy(&b, &v, 4);
    uint32_t k = g_srgb_start[b >> 15]; /* largest k with thr[k] <= v */
    while (k < 255 && g_srgb_thr[k + 1] <= v) {
        k++;
    }
    return (uint8_t)k;
}

/* Encode the clear colour for a render-target format; returns bytes per pixel, 0 if unknown. */
uint32_t gpu_encode_color(uint32_t format, const float rgba[4], uint8_t out[16]) {
    float c[4] = {rgba[0], rgba[1], rgba[2], rgba[3]};
    switch (format) {
    case 0xD6: /* A8B8G8R8_SRGB */
        pthread_once(&g_srgb_once, srgb_init);
        out[0] = srgb8(c[0]);
        out[1] = srgb8(c[1]);
        out[2] = srgb8(c[2]);
        out[3] = unorm8(c[3]);
        return 4;
    case 0xD0: /* A8R8G8B8_SRGB */
        pthread_once(&g_srgb_once, srgb_init);
        out[0] = srgb8(c[2]);
        out[1] = srgb8(c[1]);
        out[2] = srgb8(c[0]);
        out[3] = unorm8(c[3]);
        return 4;
    case 0xD5: /* A8B8G8R8_UNORM: bytes R G B A */
        out[0] = unorm8(c[0]);
        out[1] = unorm8(c[1]);
        out[2] = unorm8(c[2]);
        out[3] = unorm8(c[3]);
        return 4;
    case 0xCF: /* A8R8G8B8_UNORM: bytes B G R A */
        out[0] = unorm8(c[2]);
        out[1] = unorm8(c[1]);
        out[2] = unorm8(c[0]);
        out[3] = unorm8(c[3]);
        return 4;
    case 0xE8: /* R5G6B5_UNORM */ {
        uint16_t v = (uint16_t)(((unorm8(c[0]) >> 3) << 11) | ((unorm8(c[1]) >> 2) << 5) |
                                (unorm8(c[2]) >> 3));
        memcpy(out, &v, 2);
        return 2;
    }
    case 0xC0: /* R32G32B32A32_FLOAT */
        memcpy(out, c, 16);
        return 16;
    case 0xCA: /* R16G16B16A16_FLOAT */
        for (int i = 0; i < 4; i++) {
            uint16_t h = gpu_float_to_half(c[i]);
            memcpy(out + 2 * i, &h, 2);
        }
        return 8;
    default:
        return 0;
    }
}

uint32_t gpu_decode_color(uint32_t format, const uint8_t *in, float rgba[4]) {
    switch (format) {
    case 0xD6:
        pthread_once(&g_srgb_once, srgb_init);
        rgba[0] = g_srgb_dec[in[0]];
        rgba[1] = g_srgb_dec[in[1]];
        rgba[2] = g_srgb_dec[in[2]];
        rgba[3] = in[3] / 255.0f;
        return 4;
    case 0xD0:
        pthread_once(&g_srgb_once, srgb_init);
        rgba[0] = g_srgb_dec[in[2]];
        rgba[1] = g_srgb_dec[in[1]];
        rgba[2] = g_srgb_dec[in[0]];
        rgba[3] = in[3] / 255.0f;
        return 4;
    case 0xD5:
        for (int c = 0; c < 4; c++) {
            rgba[c] = in[c] / 255.0f;
        }
        break;
    case 0xCF:
        rgba[0] = in[2] / 255.0f;
        rgba[1] = in[1] / 255.0f;
        rgba[2] = in[0] / 255.0f;
        rgba[3] = in[3] / 255.0f;
        break;
    case 0xE8: {
        uint16_t v;
        memcpy(&v, in, 2);
        rgba[0] = ((v >> 11) & 0x1F) / 31.0f;
        rgba[1] = ((v >> 5) & 0x3F) / 63.0f;
        rgba[2] = (v & 0x1F) / 31.0f;
        rgba[3] = 1.0f;
        return 2;
    }
    case 0xC0:
        memcpy(rgba, in, 16);
        return 16;
    case 0xCA:
        for (int c = 0; c < 4; c++) {
            uint16_t h;
            memcpy(&h, in + 2 * c, 2);
            rgba[c] = gpu_half_to_float(h);
        }
        return 8;
    default:
        return 0;
    }
    return 4;
}

static void clear_surface(gpu_engines *e, uint32_t v) {
    const uint32_t *r = e->regs[ENG_3D];
    if (v & 3) {
        gpu_clear_depth_stencil(r, v & 3);
        e->stats.clears++;
    }
    if (!(v & 0x3C)) {
        return; /* depth/stencil only */
    }
    uint32_t mrt = (v >> 6) & 0xF;
    uint32_t sel = r[M3D_CT_SELECT];
    uint32_t count = sel & 0xF;
    uint32_t target = count ? (sel >> (4 + 3 * mrt)) & 7 : mrt;
    const uint32_t *rt = r + M3D_RT_BASE + M3D_RT_STRIDE * target;
    uint64_t va = addr64(rt, M3D_RT_ADDR_HI, M3D_RT_ADDR_LO);
    uint32_t width = rt[M3D_RT_WIDTH], height = rt[M3D_RT_HEIGHT], format = rt[M3D_RT_FORMAT];
    uint32_t memory = rt[M3D_RT_MEMORY];
    int pitch_layout = (memory >> 12) & 1;
    uint32_t bh = (memory >> 4) & 0xF;
    float rgba[4];
    memcpy(rgba, r + M3D_COLOR_CLEAR_VALUE, sizeof(rgba));
    uint8_t px[16];
    uint32_t bpp = gpu_encode_color(format, rgba, px);
    if (!bpp || !va || !width || !height) {
        if (!(e->warned_format & 1)) {
            e->warned_format |= 1;
            HOS_LOGW(MOD,
                     "clear: render target %u format %#x (%ux%u at %#" PRIx64 ") not supported",
                     target, format, width, height, va);
        }
        e->stats.unsupported++;
        return;
    }
    uint32_t row_bytes = pitch_layout ? width : width * bpp; /* pitch targets store the pitch */
    uint32_t px_w = pitch_layout ? width / bpp : width;
    uint32_t x0 = 0, y0 = 0, x1 = px_w, y1 = height;
    if ((r[M3D_CLEAR_CONTROL] >> 8) & 1 && (r[M3D_SCISSOR_BASE] & 1)) {
        /* The scissor is in pixels; a multisampled target is sized in samples. */
        uint32_t h = r[M3D_SCISSOR_BASE + 1], vv = r[M3D_SCISSOR_BASE + 2], gx, gy;
        gpu_ms_grid(r[0x574], &gx, &gy);
        x0 = (h & 0xFFFF) * gx;
        x1 = (h >> 16) * gx;
        y0 = (vv & 0xFFFF) * gy;
        y1 = (vv >> 16) * gy;
        x1 = x1 < px_w ? x1 : px_w;
        y1 = y1 < height ? y1 : height;
    }
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    uint64_t size =
        pitch_layout ? (uint64_t)row_bytes * height : gpu_bl_size(row_bytes, height, bh);
    uint8_t *base = nv_gpu_va_to_host(va, size);
    if (!base) {
        HOS_LOGW(MOD, "clear: render target at gpu va %#" PRIx64 " (%#" PRIx64 " bytes) not mapped",
                 va, size);
        return;
    }
    uint32_t mask = (v >> 2) & 0xF;
    int full = mask == 0xF && x0 == 0 && y0 == 0 && x1 == px_w && y1 == height;
    if (full) {
        /* Whole surface: every byte of the allocation takes the pattern, layout or not. */
        gpu_fill(base, size, px, bpp);
    } else {
        uint32_t comp = bpp >= 4 ? bpp / 4 : 0; /* bytes per component (8888, float) */
        for (uint32_t y = y0; y < y1; y++) {
            for (uint32_t x = x0; x < x1; x++) {
                uint32_t xb = x * bpp;
                uint8_t *p = base + (pitch_layout ? (uint64_t)y * row_bytes + xb
                                                  : gpu_bl_offset(xb, y, row_bytes, bh));
                if (mask == 0xF || !comp) {
                    memcpy(p, px, bpp);
                } else {
                    for (uint32_t c = 0; c < 4; c++) {
                        /* RGBA mask order; 0xCF/0xD0 store B first */
                        uint32_t slot = (format == 0xCF || format == 0xD0) && c < 3 ? 2 - c : c;
                        if (mask & (1u << c)) {
                            memcpy(p + slot * comp, px + slot * comp, comp);
                        }
                    }
                }
            }
        }
    }
    e->stats.clears++;
    HOS_LOGD(MOD, "clear rt %u %ux%u format %#x %s at %#" PRIx64 " to %02x%02x%02x%02x", target,
             px_w, height, format, pitch_layout ? "pitch" : "block-linear", va, px[0], px[1], px[2],
             px[3]);
}

/* ---- inline-to-memory --------------------------------------------------------------- */

static void i2m_launch(gpu_engines *e, int eng, uint32_t v) {
    const uint32_t *r = e->regs[eng];
    i2m_state *s = &e->i2m[eng];
    s->active = 1;
    s->launch = v;
    s->written = 0;
    s->total = (uint64_t)r[I2M_LINE_LENGTH_IN] * (r[I2M_LINE_COUNT] ? r[I2M_LINE_COUNT] : 1);
    e->stats.inline_uploads++;
}

static void i2m_data(gpu_engines *e, int eng, uint32_t v) {
    const uint32_t *r = e->regs[eng];
    i2m_state *s = &e->i2m[eng];
    if (!s->active) {
        return;
    }
    uint32_t len = r[I2M_LINE_LENGTH_IN];
    uint64_t base = addr64(r, I2M_OFFSET_OUT_UPPER, I2M_OFFSET_OUT);
    int pitch_layout = s->launch & 1;
    for (int k = 0; k < 4 && s->written < s->total; k++, s->written++) {
        uint64_t line = len ? s->written / len : 0, col = len ? s->written % len : 0;
        uint64_t va;
        if (pitch_layout) {
            va = base + line * r[I2M_PITCH_OUT] + col;
        } else {
            uint32_t bh = (r[I2M_DST_BLOCK_SIZE] >> 4) & 0xF;
            va = base + gpu_bl_offset((uint32_t)(r[I2M_DST_ORIGIN_X] + col),
                                      (uint32_t)(r[I2M_DST_ORIGIN_Y] + line), r[I2M_DST_WIDTH], bh);
        }
        uint8_t *p = nv_gpu_va_to_host(va, 1);
        if (p) {
            *p = (uint8_t)(v >> (8 * k));
        }
    }
    if (s->written >= s->total) {
        s->active = 0;
    }
}

/* ---- DMA copy ------------------------------------------------------------------------- */

/* Offset of byte (x, y) from the start of the DMA source or destination surface. */
static uint64_t dma_off(const uint32_t *r, int dst, int pitch_layout, uint32_t x, uint32_t y) {
    if (pitch_layout) {
        return (uint64_t)y * r[dst ? DMA_PITCH_OUT : DMA_PITCH_IN] + x;
    }
    uint32_t origin = r[dst ? DMA_DST_ORIGIN : DMA_SRC_ORIGIN];
    uint32_t width = r[dst ? DMA_DST_WIDTH : DMA_SRC_WIDTH];
    uint32_t bh = (r[dst ? DMA_DST_BLOCK_SIZE : DMA_SRC_BLOCK_SIZE] >> 4) & 0xF;
    return gpu_bl_offset((origin & 0xFFFF) + x, (origin >> 16) + y, width, bh);
}

/* Host pointer to the whole surface a copy touches, resolved once. */
static uint8_t *dma_surface(const uint32_t *r, int dst, int pitch_layout, uint32_t row_bytes,
                            uint32_t lines, uint64_t *span) {
    uint64_t base = dst ? addr64(r, DMA_OFFSET_OUT_HI, DMA_OFFSET_OUT_LO)
                        : addr64(r, DMA_OFFSET_IN_HI, DMA_OFFSET_IN_LO);
    if (pitch_layout) {
        uint32_t pitch = r[dst ? DMA_PITCH_OUT : DMA_PITCH_IN];
        *span = (uint64_t)(lines ? lines - 1 : 0) * pitch + row_bytes;
    } else {
        uint32_t width = r[dst ? DMA_DST_WIDTH : DMA_SRC_WIDTH];
        uint32_t height = r[dst ? DMA_DST_HEIGHT : DMA_SRC_HEIGHT];
        uint32_t bh = (r[dst ? DMA_DST_BLOCK_SIZE : DMA_SRC_BLOCK_SIZE] >> 4) & 0xF;
        *span = gpu_bl_size(width, height, bh);
    }
    return nv_gpu_va_to_host(base, *span);
}

static void dma_launch(gpu_engines *e, uint32_t v) {
    const uint32_t *r = e->regs[ENG_DMA];
    uint32_t transfer = v & 3;
    if (transfer) {
        int src_pitch = (v >> 7) & 1, dst_pitch = (v >> 8) & 1;
        int multi = (v >> 9) & 1, remap = (v >> 10) & 1;
        uint32_t lines = multi ? r[DMA_LINE_COUNT] : 1;
        uint32_t len = r[DMA_LINE_LENGTH_IN];
        /* SET_REMAP_COMPONENTS: dst x/y/z/w selectors [2:0]/[6:4]/[10:8]/[14:12] (0-3 source
         * component, 4 const A, 5 const B, 6 none), component size - 1 [17:16], source and
         * destination component counts - 1 [21:20]/[25:24]. With remap the line length
         * counts elements. */
        uint32_t rc = r[DMA_REMAP_COMPONENTS];
        uint32_t comp_size = remap ? ((rc >> 16) & 3) + 1 : 1;
        uint32_t src_comps = remap ? ((rc >> 20) & 3) + 1 : 1;
        uint32_t dst_comps = remap ? ((rc >> 24) & 3) + 1 : 1;
        uint32_t src_elem = comp_size * src_comps, dst_elem = comp_size * dst_comps;
        int needs_src = !remap;
        for (uint32_t c = 0; remap && c < dst_comps; c++) {
            needs_src |= ((rc >> (4 * c)) & 7) < 4;
        }
        uint64_t src_span = 0, dst_span = 0;
        uint8_t *dst = dma_surface(r, 1, dst_pitch, len * dst_elem, lines, &dst_span);
        const uint8_t *src =
            needs_src ? dma_surface(r, 0, src_pitch, len * src_elem, lines, &src_span) : NULL;
        if (!dst || (needs_src && !src)) {
            HOS_LOGW(MOD, "dma: %s surface not mapped (%u x %u)", dst ? "source" : "destination",
                     len, lines);
            e->stats.unsupported++;
        } else {
            for (uint32_t y = 0; y < lines; y++) {
                if (!remap && src_pitch && dst_pitch) {
                    memcpy(dst + dma_off(r, 1, 1, 0, y), src + dma_off(r, 0, 1, 0, y), len);
                    continue;
                }
                for (uint32_t x = 0; x < len; x++) {
                    uint64_t doff = dma_off(r, 1, dst_pitch, x * dst_elem, y);
                    if (doff + dst_elem > dst_span) {
                        continue;
                    }
                    if (!remap) {
                        uint64_t soff = dma_off(r, 0, src_pitch, x, y);
                        if (soff < src_span) {
                            dst[doff] = src[soff];
                        }
                        continue;
                    }
                    uint8_t in[16] = {0}, out[16];
                    if (needs_src) {
                        uint64_t soff = dma_off(r, 0, src_pitch, x * src_elem, y);
                        if (soff + src_elem <= src_span) {
                            memcpy(in, src + soff, src_elem);
                        }
                    }
                    memcpy(out, dst + doff, dst_elem);
                    for (uint32_t c = 0; c < dst_comps; c++) {
                        uint32_t sel = (rc >> (4 * c)) & 7;
                        uint8_t *o = out + c * comp_size;
                        if (sel < 4) {
                            memcpy(o, in + sel * comp_size, comp_size);
                        } else if (sel == 4 || sel == 5) {
                            uint32_t k = r[sel == 4 ? DMA_REMAP_CONST_A : DMA_REMAP_CONST_B];
                            memcpy(o, &k, comp_size);
                        }
                    }
                    memcpy(dst + doff, out, dst_elem);
                }
            }
            e->stats.copies++;
            HOS_LOGD(MOD, "dma %s -> %s, %u x %u%s", src_pitch ? "pitch" : "block-linear",
                     dst_pitch ? "pitch" : "block-linear", len, lines, remap ? " (remap)" : "");
        }
    }
    uint32_t sem = (v >> 3) & 3;
    if (sem == 1 || sem == 2) {
        if (gpu_semaphore_release(addr64(r, DMA_SEM_ADDR_HI, DMA_SEM_ADDR_LO), r[DMA_SEM_PAYLOAD],
                                  sem == 2) == 0) {
            e->stats.releases++;
        }
    }
}

/* ---- dispatch ------------------------------------------------------------------------ */

static int kind_of(uint32_t cls) {
    switch (cls) {
    case 0xB197:
        return ENG_3D;
    case 0xB1C0:
        return ENG_COMPUTE;
    case 0xA140:
        return ENG_I2M;
    case 0xB0B5:
        return ENG_DMA;
    case 0x902D:
        return ENG_2D;
    default:
        return ENG_NONE;
    }
}

gpu_engines *gpu_engines_new(void) {
    gpu_engines *e = calloc(1, sizeof(*e));
    if (e) {
        for (int i = 0; i < 8; i++) {
            e->subch[i] = ENG_NONE;
        }
    }
    return e;
}

void gpu_engines_free(gpu_engines *e) {
    free(e);
}

void gpu_engines_bind(gpu_engines *e, uint32_t subch, uint32_t cls) {
    e->subch[subch & 7] = kind_of(cls);
    if (e->subch[subch & 7] == ENG_NONE) {
        HOS_LOGW(MOD, "subchannel %u: class %#x not modelled", subch & 7, cls);
    }
}

const gpu_engine_stats *gpu_engines_stats(const gpu_engines *e) {
    return &e->stats;
}

const gpu_draw_stats *gpu_engines_draw_stats(const gpu_engines *e) {
    return &e->draw;
}

static void report_semaphore(gpu_engines *e, int eng, uint32_t v) {
    const uint32_t *r = e->regs[eng];
    uint32_t op = v & 3; /* 0 release, 1 acquire, 2 report only, 3 trap */
    if (op == 1 || op == 3) {
        e->stats.acquires_skipped++;
        return;
    }
    uint32_t payload = op == 2 ? 0 : r[M3D_REPORT_SEM_C]; /* counters: nothing was drawn */
    if (gpu_semaphore_release(addr64(r, M3D_REPORT_SEM_A, M3D_REPORT_SEM_B), payload,
                              !((v >> 28) & 1)) == 0) {
        e->stats.releases++;
    }
}

static void engine_write(gpu_engines *e, int eng, uint32_t method, uint32_t value);

static int trace_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *t = getenv("HOS_GPU_TRACE");
        on = t && *t && strcmp(t, "0") != 0;
    }
    return on;
}

static void mme_send(void *user, uint32_t method, uint32_t value) {
    if (trace_on()) {
        HOS_LOGT(MOD, "    macro -> method %#05x = %#010x", method, value);
    }
    engine_write(user, ENG_3D, method, value);
}

static uint32_t mme_read(void *user, uint32_t method) {
    gpu_engines *e = user;
    return method < METHODS ? e->regs[ENG_3D][method] : 0;
}

/* Run the macro call collected so far, if any. */
void gpu_engines_flush(gpu_engines *e) {
    mme_call *c = &e->call;
    if (!c->pending) {
        return;
    }
    c->pending = 0;
    uint32_t start = e->mme_start[c->macro % MME_MACROS];
    if (mme_execute(e->mme_code, MME_CODE_WORDS, start, c->params, c->num_params, mme_send,
                    mme_read, e) != 0) {
        HOS_LOGW(MOD, "macro %u (at %u) failed", c->macro, start);
    }
    e->stats.macros++;
}

static int mme_method(gpu_engines *e, uint32_t method, uint32_t value) {
    uint32_t *r = e->regs[ENG_3D];
    switch (method) {
    case MME_CODE_DATA:
        e->mme_code[r[MME_CODE_POINTER] % MME_CODE_WORDS] = value;
        r[MME_CODE_POINTER]++;
        return 1;
    case MME_START_DATA:
        e->mme_start[r[MME_START_POINTER] % MME_MACROS] = value;
        r[MME_START_POINTER]++;
        return 1;
    default:
        break;
    }
    if (method < MME_CALL_BASE) {
        return 0;
    }
    uint32_t macro = (method - MME_CALL_BASE) / 2;
    mme_call *c = &e->call;
    if (((method - MME_CALL_BASE) & 1) == 0 || !c->pending || c->macro != macro) {
        gpu_engines_flush(e); /* a new call: run the previous one first */
        c->pending = 1;
        c->macro = macro;
        c->num_params = 0;
    }
    if (c->num_params < MME_MAX_PARAMS) {
        c->params[c->num_params++] = value;
    }
    return 1;
}

void gpu_engines_method(gpu_engines *e, uint32_t subch, uint32_t method, uint32_t value) {
    int eng = e->subch[subch & 7];
    if (eng == ENG_NONE || method >= METHODS) {
        return;
    }
    if (eng == ENG_3D && mme_method(e, method, value)) {
        return;
    }
    gpu_engines_flush(e); /* any other method ends the parameter stream of a call */
    engine_write(e, eng, method, value);
}

/* 3D methods the draw path keeps state for, and the draw itself (M3). */
enum {
    M3D_INDEX_FIRST = 0x5F7,
    M3D_INDEX_COUNT = 0x5F8,
    M3D_VERTEX_FIRST = 0x35D,
    M3D_VERTEX_COUNT = 0x35E,
    M3D_VERTEX_END = 0x585,
    M3D_VERTEX_BEGIN = 0x586, /* [15:0] topology, [26] instance next */
    M3D_CB_SIZE = 0x8E0,
    M3D_CB_ADDR_HI = 0x8E1,
    M3D_CB_ADDR_LO = 0x8E2,
    M3D_CB_BIND = 0x904, /* + 8 * stage: [0] valid, [8:4] slot */
    M3D_CB_POS = 0x8E3,  /* byte offset of the next LOAD_CONSTANT_BUFFER word */
    M3D_CB_DATA = 0x8E4, /* 0x8E4-0x8F3: LOAD_CONSTANT_BUFFER, write and advance */
};

static void draw_method(gpu_engines *e, uint32_t method, uint32_t value) {
    uint32_t *r = e->regs[ENG_3D];
    if (method >= M3D_CB_DATA && method < M3D_CB_DATA + 16) {
        /* Constant buffer updates inside the command stream (deko3d's PushConstants). */
        uint32_t pos = r[M3D_CB_POS];
        /* Not bounded by CB_SIZE: deko3d sets the size to 0 when it only updates words
         * (texture handles), so the 64 KiB a constant buffer can span is the limit. */
        if (pos + 4 <= 0x10000) {
            uint32_t *p = nv_gpu_va_to_host(addr64(r, M3D_CB_ADDR_HI, M3D_CB_ADDR_LO) + pos, 4);
            if (p) {
                *p = value;
            }
        }
        r[M3D_CB_POS] = pos + 4;
        return;
    }
    if (method == M3D_CB_POS) {
        e->stats.inline_uploads++;
        return;
    }
    if (method >= M3D_CB_BIND && method < M3D_CB_BIND + 8 * GPU_STAGES &&
        ((method - M3D_CB_BIND) & 7) == 0) {
        uint32_t stage = (method - M3D_CB_BIND) / 8, slot = (value >> 4) & 0x1F;
        if (slot < GPU_CBUFS) {
            e->cb_addr[stage][slot] = (value & 1) ? addr64(r, M3D_CB_ADDR_HI, M3D_CB_ADDR_LO) : 0;
            e->cb_size[stage][slot] = (value & 1) ? r[M3D_CB_SIZE] : 0;
        }
    } else if (method == M3D_INDEX_COUNT) {
        e->indexed = 1;
    } else if (method == M3D_VERTEX_COUNT) {
        e->indexed = 0;
    } else if (method == M3D_VERTEX_BEGIN) {
        e->instance = ((value >> 26) & 1) ? e->instance + 1 : 0;
    } else if (method == M3D_VERTEX_END) {
        gpu_draw_state ds = {.r = r};
        memcpy(ds.cb_addr, e->cb_addr, sizeof(ds.cb_addr));
        memcpy(ds.cb_size, e->cb_size, sizeof(ds.cb_size));
        if (e->indexed) {
            gpu_draw_indexed(&ds, r[M3D_VERTEX_BEGIN] & 0xFFFF, r[M3D_INDEX_FIRST],
                             r[M3D_INDEX_COUNT], e->instance, &e->draw);
        } else {
            gpu_draw_arrays(&ds, r[M3D_VERTEX_BEGIN] & 0xFFFF, r[M3D_VERTEX_FIRST],
                            r[M3D_VERTEX_COUNT], e->instance, &e->draw);
        }
    }
}

static void engine_write(gpu_engines *e, int eng, uint32_t method, uint32_t value) {
    if (method >= METHODS) {
        return;
    }
    e->regs[eng][method] = value;
    if (eng == ENG_3D) {
        draw_method(e, method, value);
    }
    switch (eng) {
    case ENG_3D:
    case ENG_COMPUTE:
        if (method == M3D_REPORT_SEM_D) {
            report_semaphore(e, eng, value);
        } else if (method == M3D_CLEAR_SURFACE && eng == ENG_3D) {
            clear_surface(e, value);
        } else if (method == I2M_LAUNCH_DMA) {
            i2m_launch(e, eng, value);
        } else if (method == I2M_LOAD_INLINE_DATA) {
            i2m_data(e, eng, value);
        } else if (method == 0x0AF && eng == ENG_COMPUTE) { /* LAUNCH */
            gpu_compute_launch(e->regs[ENG_COMPUTE], &e->draw);
        }
        break;
    case ENG_I2M:
        if (method == I2M_LAUNCH_DMA) {
            i2m_launch(e, eng, value);
        } else if (method == I2M_LOAD_INLINE_DATA) {
            i2m_data(e, eng, value);
        }
        break;
    case ENG_DMA:
        if (method == DMA_LAUNCH) {
            dma_launch(e, value);
        }
        break;
    case ENG_2D:
        if (method == 0x237) { /* PIXELS_FROM_MEMORY_SRC_Y0_INT starts the blit */
            gpu_2d_blit(e->regs[ENG_2D]);
            e->stats.copies++;
        }
        break;
    default:
        break;
    }
}

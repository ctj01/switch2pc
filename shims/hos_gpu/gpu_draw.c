/*
 * CPU draw path for the 3D engine (M3); see gpu_draw.h. Method numbers are
 * word offsets from the Maxwell 3D class (clb197): program region 0x582,
 * pipelines 0x800 + 16i, viewports 0x280, scissors 0x380, render targets
 * 0x200, vertex attributes 0x458, vertex streams 0x700, culling 0x646-0x648,
 * colour write masks 0x680.
 */

#include "gpu_draw.h"

#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "common/hos_log.h"
#include "gpu_internal.h"
#include "gpu_shader.h"

#define MOD "draw"

enum {
    M3D_RT_BASE = 0x200,
    M3D_VIEWPORT = 0x280, /* scale x y z, translate x y z */
    M3D_SCISSOR = 0x380,  /* enable, horizontal, vertical */
    M3D_VERTEX_ATTRIB = 0x458,
    M3D_CT_SELECT = 0x487,
    M3D_PROGRAM_HI = 0x582,
    M3D_PROGRAM_LO = 0x583,
    M3D_CULL_ENABLE = 0x646,
    M3D_FRONT_FACE = 0x647,
    M3D_CULL_FACE = 0x648,
    M3D_CT_WRITE_MASK = 0x680,
    M3D_VERTEX_STREAM = 0x700, /* stride|enable, addr hi, addr lo, divisor */
    M3D_ZT_ADDR_HI = 0x3F8,    /* depth target: address, format, block size, array pitch */
    M3D_ZT_ADDR_LO = 0x3F9,
    M3D_ZT_FORMAT = 0x3FA,
    M3D_ZT_BLOCK = 0x3FB,
    M3D_ZT_WIDTH = 0x48A,
    M3D_ZT_HEIGHT = 0x48B,
    M3D_ZT_ENABLE = 0x54E,
    M3D_DEPTH_TEST = 0x4B3,
    M3D_DEPTH_WRITE = 0x4BA,
    M3D_DEPTH_FUNC = 0x4C3,
    M3D_CLEAR_DEPTH = 0x364,
    M3D_CLEAR_STENCIL = 0x368,
    M3D_BLEND_COLOR = 0x4C7,  /* constant colour r g b a */
    M3D_BLEND_ENABLE = 0x4D8, /* + render target */
    M3D_BLEND_RT = 0x780,     /* + 8 * rt: separate alpha, colour op/src/dst, alpha op/src/dst */
    M3D_PIPELINE = 0x800,     /* select, offset, ..., register count */
    SPH_SIZE = 0x50,
    MAX_VERTICES = 65536,
};

enum {
    TOPO_POINTS = 0,
    TOPO_LINES = 1,
    TOPO_TRIANGLES = 4,
    TOPO_STRIP = 5,
    TOPO_FAN = 6,
    TOPO_QUADS = 7,
};

typedef struct vertex {
    float pos[4]; /* clip space */
    float win[3]; /* window x, y, z */
    float inv_w;
    uint32_t out[SHADER_ATTR_WORDS];
} vertex;

typedef struct program {
    const uint64_t *code;
    uint32_t words;
} program;

typedef struct raster_ctx {
    const vertex *v[3];
    float l[3]; /* screen-space barycentrics */
    float px, py;
} raster_ctx;

static float fbits(uint32_t v) {
    float x;
    memcpy(&x, &v, 4);
    return x;
}

static uint32_t ubits(float x) {
    uint32_t v;
    memcpy(&v, &x, 4);
    return v;
}

static int load_program(const uint32_t *r, int type, program *p) {
    uint64_t base = ((uint64_t)(r[M3D_PROGRAM_HI] & 0xFF) << 32) | r[M3D_PROGRAM_LO];
    for (int i = 0; i < 6; i++) {
        const uint32_t *pipe = r + M3D_PIPELINE + 16 * i;
        if ((pipe[0] & 1) && ((pipe[0] >> 4) & 0xF) == (uint32_t)type) {
            uint64_t avail = 0;
            const uint8_t *host = nv_gpu_va_to_host_span(base + pipe[1] + SPH_SIZE, &avail);
            if (!host || avail < 16) {
                return 0;
            }
            p->code = (const uint64_t *)host;
            p->words = (uint32_t)(avail / 8 < 65536 ? avail / 8 : 65536);
            return 1;
        }
    }
    return 0;
}

static void bind_cbufs(const gpu_draw_state *s, int stage, shader_env *e) {
    for (int i = 0; i < GPU_CBUFS; i++) {
        e->cb[i] = NULL;
        e->cb_size[i] = 0;
        if (s->cb_size[stage][i]) {
            e->cb[i] = nv_gpu_va_to_host(s->cb_addr[stage][i], s->cb_size[stage][i]);
            e->cb_size[i] = e->cb[i] ? s->cb_size[stage][i] : 0;
        }
    }
}

/* Vertex attribute formats: size [26:21], type [29:27] (7 float, 2 unorm). */
static int fetch_attribs(const uint32_t *r, uint32_t index, uint32_t instance, shader_env *e) {
    (void)instance;
    for (int a = 0; a < 32; a++) {
        uint32_t fmt = r[M3D_VERTEX_ATTRIB + a];
        uint32_t *dst = e->in + (0x80 + 16 * a) / 4;
        dst[0] = dst[1] = dst[2] = 0;
        dst[3] = ubits(1.0f);
        if ((fmt >> 6) & 1) {
            continue; /* constant: (0, 0, 0, 1) */
        }
        uint32_t buffer = fmt & 0x1F, offset = (fmt >> 7) & 0x3FFF;
        uint32_t size = (fmt >> 21) & 0x3F, type = (fmt >> 27) & 7;
        const uint32_t *stream = r + M3D_VERTEX_STREAM + 4 * buffer;
        if (!((stream[0] >> 12) & 1)) {
            continue;
        }
        uint32_t stride = stream[0] & 0xFFF;
        uint64_t addr =
            ((uint64_t)(stream[1] & 0xFF) << 32 | stream[2]) + (uint64_t)stride * index + offset;
        int comps = size == 0x12 ? 1 : size == 0x04 ? 2 : size == 0x02 ? 3 : size == 0x01 ? 4 : 0;
        if (type == 7 && comps) {
            const uint32_t *src = nv_gpu_va_to_host(addr, (uint64_t)comps * 4);
            if (!src) {
                return 0;
            }
            for (int c = 0; c < comps; c++) {
                dst[c] = src[c];
            }
        } else if (type == 2 && size == 0x0A) { /* RGBA8 unorm */
            const uint8_t *src = nv_gpu_va_to_host(addr, 4);
            if (!src) {
                return 0;
            }
            for (int c = 0; c < 4; c++) {
                dst[c] = ubits(src[c] / 255.0f);
            }
        }
    }
    return 1;
}

static float ipa(void *user, uint32_t addr, uint32_t mode) {
    raster_ctx *c = user;
    switch (addr) {
    case 0x70:
        return c->px;
    case 0x74:
        return c->py;
    case 0x78:
        return c->l[0] * c->v[0]->win[2] + c->l[1] * c->v[1]->win[2] + c->l[2] * c->v[2]->win[2];
    case 0x7c:
        return c->l[0] * c->v[0]->inv_w + c->l[1] * c->v[1]->inv_w + c->l[2] * c->v[2]->inv_w;
    default:
        break;
    }
    uint32_t k = addr / 4;
    if (k >= SHADER_ATTR_WORDS) {
        return 0.0f;
    }
    float a0 = fbits(c->v[0]->out[k]), a1 = fbits(c->v[1]->out[k]), a2 = fbits(c->v[2]->out[k]);
    switch (mode) {
    case 1: /* perspective: attr/w, linear in screen space; the shader multiplies by w */
        return c->l[0] * a0 * c->v[0]->inv_w + c->l[1] * a1 * c->v[1]->inv_w +
               c->l[2] * a2 * c->v[2]->inv_w;
    case 2: /* flat: the provoking (last) vertex */
        return a2;
    default:
        return c->l[0] * a0 + c->l[1] * a1 + c->l[2] * a2;
    }
}

typedef struct target {
    uint8_t *base;
    uint64_t size;
    uint32_t width, height, format, bpp, row_bytes, bh;
    int pitch_layout;
    uint32_t mask; /* RGBA write mask */
    uint32_t rt;   /* render target index, for its blend state */
} target;

static int bind_target(const uint32_t *r, target *t) {
    uint32_t sel = r[M3D_CT_SELECT];
    uint32_t rt_index = (sel & 0xF) ? (sel >> 4) & 7 : 0;
    const uint32_t *rt = r + M3D_RT_BASE + 16 * rt_index;
    uint64_t va = ((uint64_t)(rt[0] & 0xFF) << 32) | rt[1];
    uint8_t probe[16];
    float zero[4] = {0, 0, 0, 0};
    t->format = rt[4];
    t->bpp = gpu_encode_color(t->format, zero, probe);
    t->pitch_layout = (rt[5] >> 12) & 1;
    t->bh = (rt[5] >> 4) & 0xF;
    t->width = t->pitch_layout && t->bpp ? rt[2] / t->bpp : rt[2];
    t->height = rt[3];
    t->row_bytes = t->pitch_layout ? rt[2] : t->width * t->bpp;
    t->rt = rt_index;
    uint32_t m = r[M3D_CT_WRITE_MASK + rt_index];
    t->mask = (m & 1) | ((m >> 3) & 2) | ((m >> 6) & 4) | ((m >> 9) & 8);
    if (!t->bpp || !va || !t->width || !t->height) {
        return 0;
    }
    t->size = t->pitch_layout ? (uint64_t)t->row_bytes * t->height
                              : gpu_bl_size(t->row_bytes, t->height, t->bh);
    t->base = nv_gpu_va_to_host(va, t->size);
    return t->base != NULL;
}

/*
 * Depth targets. How the bits are laid out only matters to this file, which is
 * the only reader: Z32F as a float, Z16 as unorm16, the 32-bit Z24 formats
 * (deko3d's Z24S8 is 0x14) as unorm24 << 8 | stencil.
 */
typedef struct depth_target {
    uint8_t *base;
    uint64_t size;
    uint32_t format, bpp, width, height, row_bytes, bh;
} depth_target;

static int bind_depth(const uint32_t *r, depth_target *d) {
    memset(d, 0, sizeof(*d));
    if (!(r[M3D_ZT_ENABLE] & 1)) {
        return 0;
    }
    d->format = r[M3D_ZT_FORMAT];
    d->bpp = d->format == 0x13                                                 ? 2
             : (d->format == 0x0A || (d->format >= 0x14 && d->format <= 0x16)) ? 4
                                                                               : 0;
    if (!d->bpp) {
        HOS_LOGW(MOD, "depth format %#x not supported", d->format);
        return 0;
    }
    d->width = r[M3D_ZT_WIDTH];
    d->height = r[M3D_ZT_HEIGHT];
    d->bh = (r[M3D_ZT_BLOCK] >> 4) & 0xF;
    d->row_bytes = d->width * d->bpp;
    d->size = gpu_bl_size(d->row_bytes, d->height, d->bh);
    uint64_t va = ((uint64_t)(r[M3D_ZT_ADDR_HI] & 0xFF) << 32) | r[M3D_ZT_ADDR_LO];
    d->base = nv_gpu_va_to_host(va, d->size);
    return d->base != NULL;
}

/* Depth in [0, 1] as the stored integer (or float bits), for exact comparisons. */
static uint32_t depth_quantize(const depth_target *d, float z) {
    z = z < 0.0f ? 0.0f : z > 1.0f ? 1.0f : z;
    switch (d->format) {
    case 0x0A:
        return ubits(z);
    case 0x13:
        return (uint32_t)((double)z * 65535.0 + 0.5);
    default:
        /* in double: z * 16777215 + 0.5 rounds to 2^24 in float and overflows the field */
        return (uint32_t)((double)z * 16777215.0 + 0.5);
    }
}

static uint32_t *depth_ptr(const depth_target *d, uint32_t x, uint32_t y) {
    uint64_t off = gpu_bl_offset(x * d->bpp, y, d->row_bytes, d->bh);
    return off + d->bpp <= d->size ? (uint32_t *)(d->base + off) : NULL;
}

static uint32_t depth_read(const depth_target *d, const uint32_t *p) {
    if (d->bpp == 2) {
        return *(const uint16_t *)p;
    }
    return d->format == 0x0A ? *p : *p >> 8;
}

static void depth_write(const depth_target *d, uint32_t *p, uint32_t q) {
    if (d->bpp == 2) {
        *(uint16_t *)p = (uint16_t)q;
    } else if (d->format == 0x0A) {
        *p = q;
    } else {
        *p = (q << 8) | (*p & 0xFF);
    }
}

/* D3D-style (deko3d) and OpenGL-style compare functions. */
static int depth_compare(uint32_t func, uint32_t z, uint32_t stored, int is_float) {
    int lt = is_float ? fbits(z) < fbits(stored) : z < stored;
    int eq = z == stored;
    if (func >= 0x200 && func <= 0x207) {
        func = func - 0x200 + 1;
    }
    switch (func) {
    case 1:
        return 0;
    case 2:
        return lt;
    case 3:
        return eq;
    case 4:
        return lt || eq;
    case 5:
        return !lt && !eq;
    case 6:
        return !eq;
    case 7:
        return !lt;
    default:
        return 1;
    }
}

void gpu_clear_depth_stencil(const uint32_t *r, uint32_t surface_bits) {
    depth_target d;
    if (!bind_depth(r, &d)) {
        return;
    }
    uint32_t q = depth_quantize(&d, fbits(r[M3D_CLEAR_DEPTH]));
    HOS_LOGD(MOD, "clear depth %#x (%g) stencil %u, format %#x, %ux%u, %#" PRIx64 " bytes", q,
             fbits(r[M3D_CLEAR_DEPTH]), r[M3D_CLEAR_STENCIL] & 0xFF, d.format, d.width, d.height,
             d.size);
    uint32_t stencil = r[M3D_CLEAR_STENCIL] & 0xFF;
    for (uint64_t off = 0; off + d.bpp <= d.size; off += d.bpp) {
        uint32_t *p = (uint32_t *)(d.base + off);
        if (surface_bits & 1) {
            depth_write(&d, p, q);
        }
        if ((surface_bits & 2) && d.bpp == 4 && d.format != 0x0A) {
            *p = (*p & ~0xFFu) | stencil;
        }
    }
}

/* Blend factor (D3D-style as deko3d writes it; 0x4000 | GL value also accepted). */
static float blend_factor(uint32_t f, int c, const float s[4], const float d[4], const float k[4]) {
    if (f & 0x4000) {
        switch (f & 0x3FFF) {
        case 0x0000:
            f = 1;
            break;
        case 0x0001:
            f = 2;
            break;
        case 0x0300:
            f = 3;
            break;
        case 0x0301:
            f = 4;
            break;
        case 0x0302:
            f = 5;
            break;
        case 0x0303:
            f = 6;
            break;
        case 0x0304:
            f = 7;
            break;
        case 0x0305:
            f = 8;
            break;
        case 0x0306:
            f = 9;
            break;
        case 0x0307:
            f = 10;
            break;
        case 0x0308:
            f = 11;
            break;
        default:
            f = 2;
            break;
        }
    }
    switch (f) {
    case 1:
        return 0.0f;
    case 2:
        return 1.0f;
    case 3:
        return s[c];
    case 4:
        return 1.0f - s[c];
    case 5:
        return s[3];
    case 6:
        return 1.0f - s[3];
    case 7:
        return d[3];
    case 8:
        return 1.0f - d[3];
    case 9:
        return d[c];
    case 10:
        return 1.0f - d[c];
    case 11:
        return c == 3 ? 1.0f : fminf(s[3], 1.0f - d[3]);
    case 0x21:
        return k[c];
    case 0x22:
        return 1.0f - k[c];
    case 0x23:
        return k[3];
    case 0x24:
        return 1.0f - k[3];
    default:
        return 1.0f;
    }
}

static float blend_op(uint32_t op, float s, float d) {
    switch (op) {
    case 2:
    case 0x800A:
        return s - d;
    case 3:
    case 0x800B:
        return d - s;
    case 4:
    case 0x8007:
        return fminf(s, d);
    case 5:
    case 0x8008:
        return fmaxf(s, d);
    default:
        return s + d;
    }
}

static void blend(const uint32_t *r, uint32_t rt, const float dst[4], float rgba[4]) {
    const uint32_t *b = r + M3D_BLEND_RT + 8 * rt;
    float k[4];
    for (int c = 0; c < 4; c++) {
        k[c] = fbits(r[M3D_BLEND_COLOR + c]);
    }
    float src[4] = {rgba[0], rgba[1], rgba[2], rgba[3]};
    for (int c = 0; c < 4; c++) {
        int alpha = c == 3;
        uint32_t op = b[alpha ? 4 : 1], sf = b[alpha ? 5 : 2], df = b[alpha ? 6 : 3];
        float fs = blend_factor(sf, c, src, dst, k), fd = blend_factor(df, c, src, dst, k);
        int minmax = op == 4 || op == 5 || op == 0x8007 || op == 0x8008;
        rgba[c] = minmax ? blend_op(op, src[c], dst[c]) : blend_op(op, src[c] * fs, dst[c] * fd);
    }
}

static void write_pixel(const target *t, uint32_t x, uint32_t y, const float rgba[4]) {
    uint8_t px[16];
    gpu_encode_color(t->format, rgba, px);
    uint64_t off = t->pitch_layout ? (uint64_t)y * t->row_bytes + (uint64_t)x * t->bpp
                                   : gpu_bl_offset(x * t->bpp, y, t->row_bytes, t->bh);
    if (off + t->bpp > t->size) {
        return;
    }
    uint8_t *p = t->base + off;
    if (t->mask == 0xF || t->bpp < 4) {
        memcpy(p, px, t->bpp);
        return;
    }
    uint32_t comp = t->bpp / 4;
    int bgra = t->format == 0xCF || t->format == 0xD0;
    for (uint32_t c = 0; c < 4; c++) {
        uint32_t slot = bgra && c < 3 ? 2 - c : c;
        if (t->mask & (1u << c)) {
            memcpy(p + slot * comp, px + slot * comp, comp);
        }
    }
}

static float edge(const float *a, const float *b, float px, float py) {
    return (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
}

static void raster_triangle(const uint32_t *r, const vertex *v0, const vertex *v1, const vertex *v2,
                            const program *fs, shader_env *env, const target *t,
                            const depth_target *dt, gpu_draw_stats *st) {
    if (v0->pos[3] <= 0.0f || v1->pos[3] <= 0.0f || v2->pos[3] <= 0.0f) {
        return; /* FIXME(#40): clip against w > 0 instead of dropping */
    }
    float area = edge(v0->win, v1->win, v2->win[0], v2->win[1]);
    if (area == 0.0f) {
        return;
    }
    /* Orientation in NDC (y up): the viewport's scale signs fold back in. */
    float sx = fbits(r[M3D_VIEWPORT + 0]), sy = fbits(r[M3D_VIEWPORT + 1]);
    int ccw = (area > 0.0f) != ((sx * sy) < 0.0f);       /* area_ndc = area_window / (sx * sy) */
    int front = r[M3D_FRONT_FACE] == 0x900 ? !ccw : ccw; /* 0x901 CCW, 0x900 CW */
    HOS_LOGT(MOD, "triangle: area %g, %s facing, cull %s face %#x", area, front ? "front" : "back",
             (r[M3D_CULL_ENABLE] & 1) ? "on" : "off", r[M3D_CULL_FACE]);
    if (r[M3D_CULL_ENABLE] & 1) {
        uint32_t cull = r[M3D_CULL_FACE];
        if (cull == 0x408 || (cull == 0x404 && front) || (cull == 0x405 && !front)) {
            return;
        }
    }
    st->triangles++;

    float minx = fminf(v0->win[0], fminf(v1->win[0], v2->win[0]));
    float maxx = fmaxf(v0->win[0], fmaxf(v1->win[0], v2->win[0]));
    float miny = fminf(v0->win[1], fminf(v1->win[1], v2->win[1]));
    float maxy = fmaxf(v0->win[1], fmaxf(v1->win[1], v2->win[1]));
    int32_t x0 = (int32_t)floorf(minx), x1 = (int32_t)ceilf(maxx);
    int32_t y0 = (int32_t)floorf(miny), y1 = (int32_t)ceilf(maxy);
    int32_t cx0 = 0, cy0 = 0, cx1 = (int32_t)t->width, cy1 = (int32_t)t->height;
    if (r[M3D_SCISSOR] & 1) {
        uint32_t h = r[M3D_SCISSOR + 1], vv = r[M3D_SCISSOR + 2];
        cx0 = (int32_t)(h & 0xFFFF);
        cx1 = (int32_t)(h >> 16) < cx1 ? (int32_t)(h >> 16) : cx1;
        cy0 = (int32_t)(vv & 0xFFFF);
        cy1 = (int32_t)(vv >> 16) < cy1 ? (int32_t)(vv >> 16) : cy1;
    }
    x0 = x0 < cx0 ? cx0 : x0;
    y0 = y0 < cy0 ? cy0 : y0;
    x1 = x1 > cx1 ? cx1 : x1;
    y1 = y1 > cy1 ? cy1 : y1;

    int depth_test = dt && dt->base && (r[M3D_DEPTH_TEST] & 1);
    int depth_write_on = dt && dt->base && (r[M3D_DEPTH_WRITE] & 1);
    int blending = r[M3D_BLEND_ENABLE + t->rt] & 1;
    uint32_t depth_func = r[M3D_DEPTH_FUNC];
    raster_ctx c = {.v = {v0, v1, v2}};
    env->ipa = ipa;
    env->user = &c;
    float inv_area = 1.0f / area;
    for (int32_t y = y0; y < y1; y++) {
        for (int32_t x = x0; x < x1; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = edge(v1->win, v2->win, px, py) * inv_area;
            float w1 = edge(v2->win, v0->win, px, py) * inv_area;
            float w2 = edge(v0->win, v1->win, px, py) * inv_area;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) {
                continue;
            }
            c.l[0] = w0;
            c.l[1] = w1;
            c.l[2] = w2;
            uint32_t *zp = NULL, zq = 0;
            if (depth_test || depth_write_on) {
                zp = depth_ptr(dt, (uint32_t)x, (uint32_t)y);
                float z = w0 * v0->win[2] + w1 * v1->win[2] + w2 * v2->win[2];
                zq = depth_quantize(dt, z);
                if (zp && depth_test &&
                    !depth_compare(depth_func, zq, depth_read(dt, zp), dt->format == 0x0A)) {
                    continue;
                }
            }
            c.px = px;
            c.py = py;
            env->killed = 0;
            uint64_t bad = 0;
            if (shader_run(fs->code, fs->words, env, &bad) != 0) {
                if (!st->shader_errors++) {
                    HOS_LOGW(MOD, "fragment shader stopped at instruction %#018" PRIx64, bad);
                }
                env->user = NULL;
                return;
            }
            if (env->killed) {
                continue;
            }
            float rgba[4] = {fbits(env->r[0]), fbits(env->r[1]), fbits(env->r[2]),
                             fbits(env->r[3])};
            if (blending) {
                uint64_t off = t->pitch_layout ? (uint64_t)y * t->row_bytes + (uint64_t)x * t->bpp
                                               : gpu_bl_offset((uint32_t)x * t->bpp, (uint32_t)y,
                                                               t->row_bytes, t->bh);
                float dst[4] = {0, 0, 0, 0};
                if (off + t->bpp <= t->size) {
                    gpu_decode_color(t->format, t->base + off, dst);
                }
                blend(r, t->rt, dst, rgba);
            }
            write_pixel(t, (uint32_t)x, (uint32_t)y, rgba);
            if (zp && depth_write_on) {
                depth_write(dt, zp, zq);
            }
            st->pixels++;
        }
    }
    env->user = NULL; /* c lives on this stack frame */
}

void gpu_draw_arrays(const gpu_draw_state *s, uint32_t topology, uint32_t first, uint32_t count,
                     uint32_t instance, gpu_draw_stats *st) {
    const uint32_t *r = s->r;
    st->draws++;
    program vs, fs;
    if (!load_program(r, 1, &vs) || !load_program(r, 5, &fs)) {
        HOS_LOGW(MOD, "draw without a vertex and a fragment program");
        return;
    }
    target t;
    if (!bind_target(r, &t)) {
        HOS_LOGW(MOD, "draw: render target format %#x not supported or not mapped", t.format);
        return;
    }
    if (count == 0 || count > MAX_VERTICES) {
        return;
    }
    depth_target dt;
    bind_depth(r, &dt);
    vertex *verts = calloc(count, sizeof(*verts));
    shader_env *env = calloc(1, sizeof(*env));
    if (!verts || !env) {
        free(verts);
        free(env);
        return;
    }

    /* Vertex stage. */
    bind_cbufs(s, 0, env);
    float sx = fbits(r[M3D_VIEWPORT + 0]), sy = fbits(r[M3D_VIEWPORT + 1]);
    float sz = fbits(r[M3D_VIEWPORT + 2]), tx = fbits(r[M3D_VIEWPORT + 3]);
    float ty = fbits(r[M3D_VIEWPORT + 4]), tz = fbits(r[M3D_VIEWPORT + 5]);
    for (uint32_t k = 0; k < count; k++) {
        memset(env->in, 0, sizeof(env->in));
        memset(env->out, 0, sizeof(env->out));
        env->in[0x2fc / 4] = first + k;
        env->in[0x2f8 / 4] = instance;
        if (!fetch_attribs(r, first + k, instance, env)) {
            HOS_LOGW(MOD, "vertex %u: attribute data not mapped", first + k);
        }
        uint64_t bad = 0;
        if (shader_run(vs.code, vs.words, env, &bad) != 0) {
            if (!st->shader_errors++) {
                HOS_LOGW(MOD, "vertex shader stopped at instruction %#018" PRIx64, bad);
            }
            free(verts);
            free(env);
            return;
        }
        vertex *v = &verts[k];
        memcpy(v->out, env->out, sizeof(v->out));
        for (int c = 0; c < 4; c++) {
            v->pos[c] = fbits(env->out[0x70 / 4 + c]);
        }
        v->inv_w = v->pos[3] != 0.0f ? 1.0f / v->pos[3] : 0.0f;
        v->win[0] = v->pos[0] * v->inv_w * sx + tx;
        v->win[1] = v->pos[1] * v->inv_w * sy + ty;
        v->win[2] = v->pos[2] * v->inv_w * sz + tz;
        if (st->draws <= 1 && k < 4) {
            HOS_LOGT(MOD, "vertex %u: clip (%g %g %g %g) window (%g %g %g) out[0x80] (%g %g %g %g)",
                     first + k, v->pos[0], v->pos[1], v->pos[2], v->pos[3], v->win[0], v->win[1],
                     v->win[2], fbits(v->out[0x20]), fbits(v->out[0x21]), fbits(v->out[0x22]),
                     fbits(v->out[0x23]));
        }
    }

    /* Primitive assembly and the fragment stage. */
    bind_cbufs(s, 4, env);
    switch (topology) {
    case TOPO_TRIANGLES:
        for (uint32_t k = 0; k + 2 < count; k += 3) {
            raster_triangle(r, &verts[k], &verts[k + 1], &verts[k + 2], &fs, env, &t, &dt, st);
        }
        break;
    case TOPO_STRIP:
        for (uint32_t k = 0; k + 2 < count; k++) {
            if (k & 1) {
                raster_triangle(r, &verts[k + 1], &verts[k], &verts[k + 2], &fs, env, &t, &dt, st);
            } else {
                raster_triangle(r, &verts[k], &verts[k + 1], &verts[k + 2], &fs, env, &t, &dt, st);
            }
        }
        break;
    case TOPO_FAN:
        for (uint32_t k = 1; k + 1 < count; k++) {
            raster_triangle(r, &verts[0], &verts[k], &verts[k + 1], &fs, env, &t, &dt, st);
        }
        break;
    case TOPO_QUADS: /* each quad as two triangles sharing its first vertex */
        for (uint32_t k = 0; k + 3 < count; k += 4) {
            raster_triangle(r, &verts[k], &verts[k + 1], &verts[k + 2], &fs, env, &t, &dt, st);
            raster_triangle(r, &verts[k], &verts[k + 2], &verts[k + 3], &fs, env, &t, &dt, st);
        }
        break;
    default:
        HOS_LOGW(MOD, "topology %u not supported yet", topology);
        break;
    }
    free(verts);
    free(env);
}

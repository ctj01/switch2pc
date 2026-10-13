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
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common/hos_log.h"
#include "gpu_internal.h"
#include "gpu_shader.h"
#include "gpu_tess.h"
#include "gpu_texture.h"

#define MOD "draw"

enum {
    M3D_TESS_MODE = 0x0C8,  /* [1:0] domain, [5:4] spacing, [8] clockwise */
    M3D_TESS_OUTER = 0x0C9, /* 4 floats, then 2 inner: the levels without a control shader */
    M3D_TESS_INNER = 0x0CD,
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
    M3D_POLYGON_MODE_FRONT = 0x36B, /* 0x1B00 point, 0x1B01 line, 0x1B02 fill */
    M3D_POLYGON_MODE_BACK = 0x36C,
    M3D_PATCH_SIZE = 0x373,
    M3D_CLEAR_DEPTH = 0x364,
    M3D_CLEAR_STENCIL = 0x368,
    M3D_BLEND_COLOR = 0x4C7,  /* constant colour r g b a */
    M3D_BLEND_ENABLE = 0x4D8, /* + render target */
    M3D_BLEND_RT = 0x780,     /* + 8 * rt: separate alpha, colour op/src/dst, alpha op/src/dst */
    M3D_TSC_POOL = 0x557,     /* sampler descriptors: address hi, lo, last index */
    M3D_TIC_POOL = 0x55D,     /* image descriptors: address hi, lo, last index */
    M3D_TEX_CB_INDEX = 0x982, /* constant buffer that holds texture handles */
    M3D_CB_SIZE = 0x8E0,
    M3D_INDEX_ADDR_HI = 0x5F2, /* index buffer: address, limit, format (0 u8, 1 u16, 2 u32) */
    M3D_INDEX_ADDR_LO = 0x5F3,
    M3D_INDEX_FORMAT = 0x5F6,
    M3D_BASE_VERTEX = 0x50D,
    M3D_MS_MODE = 0x574,
    M3D_LINE_WIDTH_ALIASED = 0x4ED,
    /* float; 0x4EC is the smooth width */ /* samples: 0 1x, 1 2x, 2 4x, 3 8x, 4 16x */
    M3D_CB_ADDR_HI = 0x8E1,
    M3D_CB_ADDR_LO = 0x8E2,
    M3D_PIPELINE = 0x800, /* select, offset, ..., register count */
    SPH_SIZE = 0x50,
    MAX_VERTICES = 65536,
    TESS_PATCH_MAX = 32, /* vertices per patch, in and out of the control shader */
};

enum {
    TOPO_POINTS = 0,
    TOPO_LINES = 1,
    TOPO_LINE_LOOP = 2,
    TOPO_LINE_STRIP = 3,
    TOPO_TRIANGLES = 4,
    TOPO_STRIP = 5,
    TOPO_FAN = 6,
    TOPO_QUADS = 7,
    TOPO_PATCHES = 14,
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
    shader_prog *prog;   /* decoded, released after the draw */
    const uint32_t *sph; /* the 0x50-byte shader program header */
} program;

/*
 * Textures. A handle (image id [19:0], sampler id [31:20]) is read from the
 * texture constant buffer; the image descriptor (TIC, 32 bytes) and sampler
 * descriptor (TSC, 32 bytes) come from the pools at 0x55D and 0x557 and are
 * decoded by gpu_texture.c. Each fragment worker keeps a small cache keyed by
 * the instruction's texture index.
 */
#define TEX_CACHE 8

typedef struct texture {
    uint32_t index; /* texture index in the instruction, cache key */
    uint32_t handle;
    gpu_texture t;
} texture;

typedef struct tex_ctx {
    const uint32_t *r;
    const uint8_t *handles;
    uint32_t handles_size;
    texture cache[TEX_CACHE];
    uint32_t used;
    uint32_t warned;
} tex_ctx;

static const void *tex_map(void *user, uint64_t va, uint64_t size) {
    (void)user;
    return nv_gpu_va_to_host(va, size);
}

static void tex_resolve(tex_ctx *t, texture *x, uint32_t index) {
    memset(x, 0, sizeof(*x));
    x->index = index;
    if ((uint64_t)index * 4 + 4 > t->handles_size || !t->handles) {
        return;
    }
    memcpy(&x->handle, t->handles + index * 4, 4);
    const uint32_t *r = t->r;
    uint32_t tic_id = x->handle & 0xFFFFF, tsc_id = x->handle >> 20;
    if (tic_id > r[M3D_TIC_POOL + 2] || tsc_id > r[M3D_TSC_POOL + 2]) {
        return;
    }
    uint64_t tic_va =
        (((uint64_t)(r[M3D_TIC_POOL] & 0xFF) << 32) | r[M3D_TIC_POOL + 1]) + (uint64_t)tic_id * 32;
    uint64_t tsc_va =
        (((uint64_t)(r[M3D_TSC_POOL] & 0xFF) << 32) | r[M3D_TSC_POOL + 1]) + (uint64_t)tsc_id * 32;
    const uint32_t *tic = nv_gpu_va_to_host(tic_va, 32), *tsc = nv_gpu_va_to_host(tsc_va, 32);
    if (!tic || !tsc) {
        return;
    }
    if (!gpu_tex_init(&x->t, tic, tsc, tex_map, NULL) && !(t->warned & 1)) {
        t->warned |= 1;
        HOS_LOGW(MOD, "texture format %#x layout %u (%ux%u, %u levels) not supported", x->t.format,
                 (tic[2] >> 21) & 7, x->t.width, x->t.height, x->t.levels);
    }
}

static texture *tex_lookup(tex_ctx *t, uint32_t index) {
    for (uint32_t k = 0; k < t->used; k++) {
        if (t->cache[k].index == index) {
            return &t->cache[k];
        }
    }
    texture *x = &t->cache[t->used < TEX_CACHE ? t->used++ : TEX_CACHE - 1];
    tex_resolve(t, x, index);
    return x;
}

static void tex_sample(void *user, uint32_t index, const shader_tex_coord *c, float out[4]) {
    texture *x = tex_lookup(user, index);
    if (!x->t.valid) {
        out[0] = out[1] = out[2] = 0.0f;
        out[3] = 1.0f;
        return;
    }
    gpu_tex_sample(&x->t, c, out);
}

static void tex_texel(void *user, uint32_t index, int32_t ix, int32_t iy, uint32_t level,
                      float out[4]) {
    texture *x = tex_lookup(user, index);
    if (!x->t.valid) {
        out[0] = out[1] = out[2] = 0.0f;
        out[3] = 1.0f;
        return;
    }
    gpu_tex_fetch(&x->t, level, ix, iy, out);
}

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

/* Clip space to window coordinates, as the viewport (in samples) maps them. */
typedef struct xform {
    float sx, sy, sz, tx, ty, tz;
} xform;

static void finish_vertex(vertex *v, const uint32_t *out, const xform *x) {
    memcpy(v->out, out, sizeof(v->out));
    for (int c = 0; c < 4; c++) {
        v->pos[c] = fbits(out[0x70 / 4 + c]);
    }
    v->inv_w = v->pos[3] != 0.0f ? 1.0f / v->pos[3] : 0.0f;
    v->win[0] = v->pos[0] * v->inv_w * x->sx + x->tx;
    v->win[1] = v->pos[1] * v->inv_w * x->sy + x->ty;
    v->win[2] = v->pos[2] * v->inv_w * x->sz + x->tz;
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
            p->sph = (const uint32_t *)nv_gpu_va_to_host(base + pipe[1], SPH_SIZE);
            p->words = (uint32_t)(avail / 8 < 65536 ? avail / 8 : 65536);
            p->prog = shader_prog_get(p->code, p->words);
            return p->prog != NULL;
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

/* The colour targets a draw writes: CT_SELECT [3:0] counts them, [6:4], [9:7].. index them. */
typedef struct targets {
    target t[8];
    uint32_t n;
} targets;

static int bind_target(const uint32_t *r, uint32_t k, target *t) {
    uint32_t sel = r[M3D_CT_SELECT];
    uint32_t rt_index = (sel & 0xF) ? (sel >> (4 + 3 * k)) & 7 : k;
    t->base = NULL;
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
    int has_stencil = d.bpp == 4 && d.format != 0x0A;
    if ((surface_bits & 1) && (!has_stencil || (surface_bits & 2))) {
        /* Every element becomes the same value: build it once and fill. */
        uint32_t px = 0;
        depth_write(&d, &px, q);
        if (has_stencil) {
            px = (px & ~0xFFu) | stencil;
        }
        gpu_fill(d.base, d.size, (const uint8_t *)&px, d.bpp);
        return;
    }
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

/* Facing of a window-space triangle: 1 front, 0 back, -1 for no area. */
static int tri_front(const uint32_t *r, const vertex *v0, const vertex *v1, const vertex *v2) {
    float area = edge(v0->win, v1->win, v2->win[0], v2->win[1]);
    if (area == 0.0f) {
        return -1;
    }
    /* Orientation in NDC (y up): the viewport's scale signs fold back in. */
    float sx = fbits(r[M3D_VIEWPORT + 0]), sy = fbits(r[M3D_VIEWPORT + 1]);
    int ccw = (area > 0.0f) != ((sx * sy) < 0.0f);  /* area_ndc = area_window / (sx * sy) */
    return r[M3D_FRONT_FACE] == 0x900 ? !ccw : ccw; /* 0x901 CCW, 0x900 CW */
}

static int culled(const uint32_t *r, int front) {
    if (!(r[M3D_CULL_ENABLE] & 1)) {
        return 0;
    }
    uint32_t cull = r[M3D_CULL_FACE];
    return cull == 0x408 || (cull == 0x404 && front) || (cull == 0x405 && !front);
}

/* Multisampled targets hold a grid of samples per pixel; we rasterise at sample resolution
 * (one shading per sample), so window coordinates and scissors scale by this grid. */
void gpu_ms_grid(uint32_t mode, uint32_t *gx, uint32_t *gy) {
    static const uint8_t grid[5][2] = {{1, 1}, {2, 1}, {2, 2}, {4, 2}, {4, 4}};
    mode = mode < 5 ? mode : 0;
    *gx = grid[mode][0];
    *gy = grid[mode][1];
}

#define BAND_ROWS 8 /* rows per fragment-worker band */

/*
 * Pixels are shaded in 2x2 quads, two quads per group of SHADER_LANES, as the
 * hardware does: a quad with any covered pixel runs all four lanes, so the
 * texture units get the coordinate derivatives across the quad, and the
 * uncovered lanes (helpers) write nothing. Each quad waits with its coverage,
 * depth and interpolation state until the group is full or the triangle ends,
 * then the group runs and its colours and depths are written in the order the
 * pixels were found. Pixels of one triangle never overlap, so deferring their
 * writes changes nothing.
 */
typedef struct pending_frag {
    int32_t bx, by;
    uint32_t cover, zq[16];
    uint32_t *zp[16];
} pending_frag;

typedef struct frag_batch {
    const uint32_t *r;
    const program *fs;
    const shader_env *env;
    shader_group *g;
    const targets *ts;
    const depth_target *dt;
    gpu_draw_stats *st;
    uint32_t omap, first_reg[8], gx, ns;
    int depth_write_on;
    uint32_t n;
    pending_frag frag[SHADER_LANES];
    raster_ctx ctx[SHADER_LANES];
} frag_batch;

static float ipa_lane(void *user, uint32_t lane, uint32_t addr, uint32_t mode) {
    return ipa(&((raster_ctx *)user)[lane], addr, mode);
}

/* Shade and write the pending pixels; -1 when the shader stops. */
static int flush_batch(frag_batch *b) {
    if (!b->n) {
        return 0;
    }
    const uint32_t *r = b->r;
    shader_group *g = b->g;
    g->ipa = ipa_lane;
    g->user = b->ctx;
    uint64_t bad = 0;
    uint32_t n = b->n;
    b->n = 0;
    if (shader_exec_group(b->fs->prog, g, (1u << n) - 1, b->env, &bad) != 0) {
        if (!b->st->shader_errors++) {
            HOS_LOGW(MOD, "fragment shader stopped at instruction %#018" PRIx64, bad);
        }
        return -1;
    }
    for (uint32_t q = 0; q < n; q++) {
        const pending_frag *fr = &b->frag[q];
        for (uint32_t t_i = 0; t_i < b->ts->n; t_i++) {
            const target *tk = &b->ts->t[t_i];
            uint32_t m = (b->omap >> (4 * tk->rt)) & 0xF, rn = b->first_reg[tk->rt];
            if (!tk->base || !m) {
                continue;
            }
            float src[4] = {0, 0, 0, 0};
            for (uint32_t ch = 0; ch < 4; ch++) {
                if (m & (1u << ch)) {
                    src[ch] = fbits(g->r[rn++][q]);
                }
            }
            int blending = r[M3D_BLEND_ENABLE + tk->rt] & 1;
            for (uint32_t k = 0; k < b->ns; k++) {
                if (!(fr->cover & (1u << k))) {
                    continue;
                }
                uint32_t x = (uint32_t)fr->bx + k % b->gx, y = (uint32_t)fr->by + k / b->gx;
                float rgba[4] = {src[0], src[1], src[2], src[3]};
                if (blending) {
                    uint64_t off = tk->pitch_layout
                                       ? (uint64_t)y * tk->row_bytes + (uint64_t)x * tk->bpp
                                       : gpu_bl_offset(x * tk->bpp, y, tk->row_bytes, tk->bh);
                    float dst[4] = {0, 0, 0, 0};
                    if (off + tk->bpp <= tk->size) {
                        gpu_decode_color(tk->format, tk->base + off, dst);
                    }
                    blend(r, tk->rt, dst, rgba);
                }
                write_pixel(tk, x, y, rgba);
            }
        }
        for (uint32_t k = 0; k < b->ns; k++) {
            if ((fr->cover & (1u << k)) && fr->zp[k] && b->depth_write_on) {
                depth_write(b->dt, fr->zp[k], fr->zq[k]);
            }
        }
        b->st->pixels += (uint64_t)__builtin_popcount(fr->cover);
    }
    return 0;
}

static void raster_triangle(const uint32_t *r, const vertex *v0, const vertex *v1, const vertex *v2,
                            const program *fs, shader_env *env, shader_group *grp,
                            const targets *ts, const depth_target *dt, int may_cull, uint32_t band,
                            uint32_t nbands, gpu_draw_stats *st) {
    const target *t = &ts->t[0];
    if (v0->pos[3] <= 0.0f || v1->pos[3] <= 0.0f || v2->pos[3] <= 0.0f) {
        return; /* FIXME(#40): clip against w > 0 instead of dropping */
    }
    float area = edge(v0->win, v1->win, v2->win[0], v2->win[1]);
    int front = tri_front(r, v0, v1, v2);
    if (front < 0) {
        return;
    }
    HOS_LOGT(MOD, "triangle: area %g, %s facing, cull %s face %#x", area, front ? "front" : "back",
             (r[M3D_CULL_ENABLE] & 1) ? "on" : "off", r[M3D_CULL_FACE]);
    if (may_cull && culled(r, front)) {
        return;
    }
    if (band == 0) {
        st->triangles++; /* every band sees every triangle; count it once */
    }

    float minx = fminf(v0->win[0], fminf(v1->win[0], v2->win[0]));
    float maxx = fmaxf(v0->win[0], fmaxf(v1->win[0], v2->win[0]));
    float miny = fminf(v0->win[1], fminf(v1->win[1], v2->win[1]));
    float maxy = fmaxf(v0->win[1], fmaxf(v1->win[1], v2->win[1]));
    int32_t x0 = (int32_t)floorf(minx), x1 = (int32_t)ceilf(maxx);
    int32_t y0 = (int32_t)floorf(miny), y1 = (int32_t)ceilf(maxy);
    int32_t cx0 = 0, cy0 = 0, cx1 = (int32_t)t->width, cy1 = (int32_t)t->height;
    if (r[M3D_SCISSOR] & 1) {
        uint32_t h = r[M3D_SCISSOR + 1], vv = r[M3D_SCISSOR + 2], gx, gy;
        gpu_ms_grid(r[M3D_MS_MODE], &gx, &gy);
        int32_t sx1 = (int32_t)((h >> 16) * gx), sy1 = (int32_t)((vv >> 16) * gy);
        cx0 = (int32_t)((h & 0xFFFF) * gx);
        cx1 = sx1 < cx1 ? sx1 : cx1;
        cy0 = (int32_t)((vv & 0xFFFF) * gy);
        cy1 = sy1 < cy1 ? sy1 : cy1;
    }
    x0 = x0 < cx0 ? cx0 : x0;
    y0 = y0 < cy0 ? cy0 : y0;
    x1 = x1 > cx1 ? cx1 : x1;
    y1 = y1 > cy1 ? cy1 : y1;

    int depth_test = dt && dt->base && (r[M3D_DEPTH_TEST] & 1);
    int depth_write_on = dt && dt->base && (r[M3D_DEPTH_WRITE] & 1);
    /* Fragment outputs: each target's enabled components (SPH word 18, 4 bits per
     * target) take the next registers from R0 up. */
    uint32_t omap = fs->sph ? fs->sph[18] : 0xF, first_reg[8], reg_n = 0;
    for (uint32_t k = 0; k < 8; k++) {
        first_reg[k] = reg_n;
        for (uint32_t c = 0; c < 4; c++) {
            reg_n += (omap >> (4 * k + c)) & 1;
        }
    }
    uint32_t depth_func = r[M3D_DEPTH_FUNC];
    float inv_area = 1.0f / area;
    /* Multisampling shades once per pixel, as the hardware does: each sample of the
     * pixel (a gx x gy block of the target) is covered and depth-tested on its own,
     * the shader runs once at the pixel centre, and its colour goes to every sample
     * that passed. With one sample per pixel this is the plain loop. */
    uint32_t gx, gy;
    gpu_ms_grid(r[M3D_MS_MODE], &gx, &gy);
    uint32_t ns = gx * gy;
    frag_batch *fb = malloc(sizeof(*fb));
    if (!fb) {
        return;
    }
    fb->r = r;
    fb->fs = fs;
    fb->env = env;
    fb->g = grp;
    fb->ts = ts;
    fb->dt = dt;
    fb->st = st;
    fb->omap = omap;
    memcpy(fb->first_reg, first_reg, sizeof(first_reg));
    fb->gx = gx;
    fb->ns = ns;
    fb->depth_write_on = depth_write_on;
    fb->n = 0;
    /* A quad is 2x2 pixels, 2gx x 2gy samples; BAND_ROWS is a multiple of 2gy, so a
     * quad never straddles two workers' bands. */
    int32_t qw = 2 * (int32_t)gx, qh = 2 * (int32_t)gy;
    int32_t qx0 = x0 - x0 % qw, qy0 = y0 - y0 % qh;
    for (int32_t qy = qy0; qy < y1; qy += qh) {
        if (((uint32_t)qy / BAND_ROWS) % nbands != band) {
            continue; /* another worker's rows */
        }
        for (int32_t qx = qx0; qx < x1; qx += qw) {
            uint32_t any = 0;
            for (uint32_t lane = 0; lane < 4; lane++) {
                int32_t bx = qx + (int32_t)(lane & 1) * (int32_t)gx;
                int32_t by = qy + (int32_t)(lane >> 1) * (int32_t)gy;
                pending_frag *fr = &fb->frag[fb->n + lane];
                uint32_t cover = 0;
                for (uint32_t k = 0; k < ns; k++) {
                    int32_t x = bx + (int32_t)(k % gx), y = by + (int32_t)(k / gx);
                    if (x < x0 || x >= x1 || y < y0 || y >= y1) {
                        continue;
                    }
                    float px = (float)x + 0.5f, py = (float)y + 0.5f;
                    float w0 = edge(v1->win, v2->win, px, py) * inv_area;
                    float w1 = edge(v2->win, v0->win, px, py) * inv_area;
                    float w2 = edge(v0->win, v1->win, px, py) * inv_area;
                    if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) {
                        continue;
                    }
                    fr->zp[k] = NULL;
                    fr->zq[k] = 0;
                    if (depth_test || depth_write_on) {
                        fr->zp[k] = depth_ptr(dt, (uint32_t)x, (uint32_t)y);
                        float z = w0 * v0->win[2] + w1 * v1->win[2] + w2 * v2->win[2];
                        fr->zq[k] = depth_quantize(dt, z);
                        if (fr->zp[k] && depth_test &&
                            !depth_compare(depth_func, fr->zq[k], depth_read(dt, fr->zp[k]),
                                           dt->format == 0x0A)) {
                            continue;
                        }
                    }
                    cover |= 1u << k;
                }
                /* Attributes at the pixel centre, covered or not: a helper lane only
                 * feeds derivatives. gl_FragCoord is in pixels. */
                raster_ctx *c = &fb->ctx[fb->n + lane];
                c->v[0] = v0;
                c->v[1] = v1;
                c->v[2] = v2;
                float cx = (float)bx + 0.5f * (float)gx, cy = (float)by + 0.5f * (float)gy;
                c->l[0] = edge(v1->win, v2->win, cx, cy) * inv_area;
                c->l[1] = edge(v2->win, v0->win, cx, cy) * inv_area;
                c->l[2] = edge(v0->win, v1->win, cx, cy) * inv_area;
                c->px = cx / (float)gx;
                c->py = cy / (float)gy;
                fr->bx = bx;
                fr->by = by;
                fr->cover = cover;
                any |= cover;
            }
            if (!any) {
                continue;
            }
            fb->n += 4;
            if (fb->n == SHADER_LANES && flush_batch(fb) != 0) {
                free(fb);
                return;
            }
        }
    }
    flush_batch(fb);
    free(fb);
}

/*
 * Fragment workers. The fragment stage splits the target into bands of
 * BAND_ROWS rows dealt round-robin to the workers; every worker walks every
 * triangle in order but shades only its own rows, so each pixel still sees
 * primitives in API order (depth and blending stay exact) with no locking.
 * Each worker has its own shader registers and texture cache. HOS_GPU_THREADS
 * sets the count (default: online CPUs, at most MAX_WORKERS; 1 is serial).
 */
#define MAX_WORKERS 32

typedef struct frag_tri {
    const vertex *v[3];
    int no_cull; /* a line's rectangle: facing does not apply */
} frag_tri;

typedef struct frag_job {
    const uint32_t *r;
    const program *fs;
    const shader_env *env; /* template: bound constant buffers and callbacks */
    const tex_ctx *tc;     /* template texture context, may be NULL */
    const targets *ts;
    const depth_target *dt;
    const frag_tri *tris;
    uint32_t ntris;
    uint64_t errors; /* shader errors so far: only the first ever is logged */
    gpu_draw_stats st[MAX_WORKERS];
} frag_job;

static void frag_worker(void *arg, uint32_t band, uint32_t nbands) {
    frag_job *j = arg;
    gpu_draw_stats *st = &j->st[band];
    shader_env *env = malloc(sizeof(*env));
    shader_group *grp = malloc(sizeof(*grp));
    tex_ctx *tc = j->tc ? malloc(sizeof(*tc)) : NULL;
    if (!env || !grp || (j->tc && !tc)) {
        free(env);
        free(grp);
        free(tc);
        return;
    }
    memcpy(env, j->env, sizeof(*env));
    if (tc) {
        memcpy(tc, j->tc, sizeof(*tc));
        env->tex_user = tc;
    }
    st->shader_errors = j->errors;
    for (uint32_t k = 0; k < j->ntris; k++) {
        const frag_tri *t = &j->tris[k];
        raster_triangle(j->r, t->v[0], t->v[1], t->v[2], j->fs, env, grp, j->ts, j->dt, !t->no_cull,
                        band, nbands, st);
    }
    st->shader_errors -= j->errors;
    free(tc);
    free(grp);
    free(env);
}

typedef void (*pool_fn)(void *arg, uint32_t band, uint32_t nbands);

static struct {
    pthread_mutex_t run; /* one job at a time (several channels may submit) */
    pthread_mutex_t lock;
    pthread_cond_t start, done;
    uint32_t n; /* workers, the caller included */
    uint64_t gen;
    uint32_t pending;
    pool_fn fn;
    void *arg;
} g_pool = {.run = PTHREAD_MUTEX_INITIALIZER,
            .lock = PTHREAD_MUTEX_INITIALIZER,
            .start = PTHREAD_COND_INITIALIZER,
            .done = PTHREAD_COND_INITIALIZER};
static pthread_once_t g_pool_once = PTHREAD_ONCE_INIT;

static void *pool_main(void *arg) {
    uint32_t band = (uint32_t)(uintptr_t)arg;
    uint64_t seen = 0;
    for (;;) {
        pthread_mutex_lock(&g_pool.lock);
        while (g_pool.gen == seen) {
            pthread_cond_wait(&g_pool.start, &g_pool.lock);
        }
        seen = g_pool.gen;
        pool_fn fn = g_pool.fn;
        void *job = g_pool.arg;
        uint32_t n = g_pool.n;
        pthread_mutex_unlock(&g_pool.lock);
        fn(job, band, n);
        pthread_mutex_lock(&g_pool.lock);
        if (--g_pool.pending == 0) {
            pthread_cond_signal(&g_pool.done);
        }
        pthread_mutex_unlock(&g_pool.lock);
    }
    return NULL;
}

static void pool_init(void) {
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    const char *env = getenv("HOS_GPU_THREADS");
    long n = env && *env ? strtol(env, NULL, 0) : cpus;
    n = n < 1 ? 1 : n > MAX_WORKERS ? MAX_WORKERS : n;
    g_pool.n = (uint32_t)n; /* before any worker reads it */
    for (long k = 1; k < n; k++) {
        pthread_t t;
        if (pthread_create(&t, NULL, pool_main, (void *)(uintptr_t)k) != 0) {
            g_pool.n = (uint32_t)k; /* workers k.. never started; none ran a job yet */
            break;
        }
        pthread_detach(t);
    }
    HOS_LOGI(MOD, "fragment stage on %u threads", g_pool.n);
}

/* Run fn(arg, band, n) for band 0..n-1 on the pool; the caller takes band 0. */
static void pool_run(pool_fn fn, void *arg) {
    pthread_once(&g_pool_once, pool_init);
    pthread_mutex_lock(&g_pool.run);
    uint32_t n = g_pool.n;
    if (n > 1) {
        pthread_mutex_lock(&g_pool.lock);
        g_pool.fn = fn;
        g_pool.arg = arg;
        g_pool.pending = n - 1;
        g_pool.gen++;
        pthread_cond_broadcast(&g_pool.start);
        pthread_mutex_unlock(&g_pool.lock);
    }
    fn(arg, 0, n);
    if (n > 1) {
        pthread_mutex_lock(&g_pool.lock);
        while (g_pool.pending) {
            pthread_cond_wait(&g_pool.done, &g_pool.lock);
        }
        pthread_mutex_unlock(&g_pool.lock);
    }
    pthread_mutex_unlock(&g_pool.run);
}

static void draw_common(const gpu_draw_state *s, uint32_t topology, const uint32_t *indices,
                        uint32_t first, uint32_t count, uint32_t instance, gpu_draw_stats *st);

void gpu_draw_arrays(const gpu_draw_state *s, uint32_t topology, uint32_t first, uint32_t count,
                     uint32_t instance, gpu_draw_stats *st) {
    draw_common(s, topology, NULL, first, count, instance, st);
}

void gpu_draw_indexed(const gpu_draw_state *s, uint32_t topology, uint32_t first, uint32_t count,
                      uint32_t instance, gpu_draw_stats *st) {
    const uint32_t *r = s->r;
    if (count == 0 || count > MAX_VERTICES) {
        return;
    }
    uint32_t fmt = r[M3D_INDEX_FORMAT], size = fmt == 0 ? 1 : fmt == 1 ? 2 : 4;
    uint64_t va = (((uint64_t)(r[M3D_INDEX_ADDR_HI] & 0xFF) << 32) | r[M3D_INDEX_ADDR_LO]) +
                  (uint64_t)first * size;
    const uint8_t *src = nv_gpu_va_to_host(va, (uint64_t)count * size);
    uint32_t *idx = malloc((size_t)count * 4);
    if (!src || !idx) {
        HOS_LOGW(MOD, "index buffer at %#" PRIx64 " not mapped", va);
        free(idx);
        return;
    }
    int32_t base = (int32_t)r[M3D_BASE_VERTEX];
    for (uint32_t k = 0; k < count; k++) {
        uint32_t v = size == 1   ? src[k]
                     : size == 2 ? ((const uint16_t *)src)[k]
                                 : ((const uint32_t *)src)[k];
        idx[k] = (uint32_t)((int32_t)v + base);
    }
    draw_common(s, topology, idx, 0, count, instance, st);
    free(idx);
}

#define LINE_CHUNK 2048 /* line segments rasterised per batch */

/* Shade `tris` with the job's state on the worker pool and add up the statistics. */
static void run_frag_job(frag_job *job, const frag_tri *tris, uint32_t ntris, gpu_draw_stats *st) {
    job->tris = tris;
    job->ntris = ntris;
    job->errors = st->shader_errors;
    memset(job->st, 0, sizeof(job->st));
    pool_run(frag_worker, job);
    for (uint32_t w = 0; w < MAX_WORKERS; w++) {
        st->triangles += job->st[w].triangles;
        st->pixels += job->st[w].pixels;
        st->shader_errors += job->st[w].shader_errors;
    }
}

typedef struct programs {
    program vs, tcs, tes, fs; /* tcs and tes are optional: prog is NULL when not bound */
} programs;

static void draw_programs(const gpu_draw_state *s, uint32_t topology, const uint32_t *indices,
                          uint32_t first, uint32_t count, uint32_t instance, const programs *pg,
                          gpu_draw_stats *st);

static void draw_common(const gpu_draw_state *s, uint32_t topology, const uint32_t *indices,
                        uint32_t first, uint32_t count, uint32_t instance, gpu_draw_stats *st) {
    st->draws++;
    programs pg;
    memset(&pg, 0, sizeof(pg));
    if (load_program(s->r, 1, &pg.vs) && load_program(s->r, 5, &pg.fs)) {
        if (topology == TOPO_PATCHES) {
            load_program(s->r, 2, &pg.tcs);
            load_program(s->r, 3, &pg.tes);
        }
        draw_programs(s, topology, indices, first, count, instance, &pg, st);
    } else {
        HOS_LOGW(MOD, "draw without a vertex and a fragment program");
    }
    shader_prog_release(pg.vs.prog);
    shader_prog_release(pg.tcs.prog);
    shader_prog_release(pg.tes.prog);
    shader_prog_release(pg.fs.prog);
}

/*
 * Tessellation (M5). Every `psize` shaded vertices form a patch. The control
 * shader runs once per output vertex over it, with the invocation id, the
 * inputs by vertex index and the shared patch space where it leaves the
 * levels; the primitive generator turns the levels into tessellation
 * coordinates and primitives; the evaluation shader runs at each coordinate
 * (0x2f0, 0x2f4) with the control shader's outputs as its input vertices.
 * Without a control shader the levels come from 0x0c9-0x0ce and the patch's
 * own vertices feed the evaluation shader. Returns 1 with the generated
 * vertices and the primitive list (triangles, or line segments for isolines),
 * 0 when nothing was generated.
 */
static int tessellate(const gpu_draw_state *s, const programs *pg, const vertex *const *vp,
                      uint32_t count, shader_env *env, const xform *xf, vertex **out_verts,
                      const vertex ***out_prim, uint32_t *out_n, int *out_lines,
                      gpu_draw_stats *st) {
    const uint32_t *r = s->r;
    uint32_t psize = r[M3D_PATCH_SIZE] & 0x3F;
    psize = psize == 0 ? 1 : psize > TESS_PATCH_MAX ? TESS_PATCH_MAX : psize;
    uint32_t mode = r[M3D_TESS_MODE], domain = mode & 3, spacing = (mode >> 4) & 3;
    int cw = (mode >> 8) & 1;
    uint32_t nout = pg->tcs.prog && pg->tcs.sph ? pg->tcs.sph[2] >> 24 : psize;
    nout = nout == 0 ? psize : nout > TESS_PATCH_MAX ? TESS_PATCH_MAX : nout;
    uint32_t *tcs_out = calloc((size_t)nout * SHADER_ATTR_WORDS, sizeof(*tcs_out));
    uint32_t *patch = calloc(SHADER_ATTR_WORDS, sizeof(*patch));
    const uint32_t *in_ptrs[TESS_PATCH_MAX], *tcs_ptrs[TESS_PATCH_MAX];
    vertex *tv = NULL;
    uint32_t *tidx = NULL, nv = 0, ni = 0, cap_v = 0, cap_i = 0;
    int lines = domain == TESS_ISOLINES, ok = tcs_out && patch;
    if (domain != TESS_ISOLINES && !(mode & 0x200)) {
        /* [9] clear is point mode for triangles and quads (Mesa sets CONNECTED for
         * every other output primitive); points are not generated yet. */
        HOS_LOGW(MOD, "tessellation point mode not supported; drawing triangles");
    }
    for (uint32_t k = 0; k < nout; k++) {
        tcs_ptrs[k] = tcs_out + (size_t)k * SHADER_ATTR_WORDS;
    }
    for (uint32_t p = 0; ok && (uint64_t)(p + 1) * psize <= count; p++) {
        for (uint32_t k = 0; k < psize; k++) {
            in_ptrs[k] = vp[p * psize + k]->out;
        }
        memset(patch, 0, SHADER_ATTR_WORDS * sizeof(*patch));
        float outer[4], inner[2];
        const uint32_t *const *tes_in = in_ptrs;
        uint32_t tes_n = psize;
        uint64_t bad = 0;
        if (pg->tcs.prog) {
            bind_cbufs(s, 1, env);
            env->in_vertices = in_ptrs;
            env->num_in_vertices = psize;
            env->patch = patch;
            for (uint32_t inv = 0; inv < nout; inv++) {
                memset(env->in, 0, sizeof(env->in));
                memset(env->out, 0, sizeof(env->out));
                env->invocation_id = inv;
                if (shader_exec(pg->tcs.prog, env, &bad) != 0) {
                    if (!st->shader_errors++) {
                        HOS_LOGW(MOD,
                                 "tessellation control shader stopped at instruction %#018" PRIx64,
                                 bad);
                    }
                    ok = 0;
                    break;
                }
                memcpy(tcs_out + (size_t)inv * SHADER_ATTR_WORDS, env->out, sizeof(env->out));
            }
            if (!ok) {
                break;
            }
            for (int e = 0; e < 4; e++) {
                outer[e] = fbits(patch[e]);
            }
            inner[0] = fbits(patch[4]);
            inner[1] = fbits(patch[5]);
            tes_in = tcs_ptrs;
            tes_n = nout;
        } else {
            for (int e = 0; e < 4; e++) {
                outer[e] = fbits(r[M3D_TESS_OUTER + e]);
            }
            inner[0] = fbits(r[M3D_TESS_INNER]);
            inner[1] = fbits(r[M3D_TESS_INNER + 1]);
        }
        tess_output to;
        if (tess_generate(domain, spacing, cw, outer, inner, &to) != 0) {
            HOS_LOGW(MOD, "tessellation mode %#x not supported", mode);
            ok = 0;
            break;
        }
        if (nv + to.ncoords > MAX_VERTICES) {
            HOS_LOGW(MOD, "tessellation: more than %u vertices in one draw", MAX_VERTICES);
            tess_free(&to);
            break;
        }
        if (nv + to.ncoords > cap_v) {
            uint32_t cap = cap_v ? 2 * cap_v : 1024;
            cap = cap < nv + to.ncoords ? nv + to.ncoords : cap;
            vertex *grown = realloc(tv, (size_t)cap * sizeof(*tv));
            if (!grown) {
                ok = 0;
                tess_free(&to);
                break;
            }
            tv = grown;
            cap_v = cap;
        }
        if (ni + to.nindices > cap_i) {
            uint32_t cap = cap_i ? 2 * cap_i : 4096;
            cap = cap < ni + to.nindices ? ni + to.nindices : cap;
            uint32_t *grown = realloc(tidx, (size_t)cap * sizeof(*tidx));
            if (!grown) {
                ok = 0;
                tess_free(&to);
                break;
            }
            tidx = grown;
            cap_i = cap;
        }
        bind_cbufs(s, 2, env);
        env->in_vertices = tes_in;
        env->num_in_vertices = tes_n;
        env->patch = patch;
        env->invocation_id = 0;
        uint32_t base = nv;
        for (uint32_t c = 0; c < to.ncoords && ok; c++) {
            memset(env->in, 0, sizeof(env->in));
            memset(env->out, 0, sizeof(env->out));
            env->in[0x2f0 / 4] = env->out[0x2f0 / 4] = ubits(to.coords[c].u);
            env->in[0x2f4 / 4] = env->out[0x2f4 / 4] = ubits(to.coords[c].v);
            if (shader_exec(pg->tes.prog, env, &bad) != 0) {
                if (!st->shader_errors++) {
                    HOS_LOGW(MOD,
                             "tessellation evaluation shader stopped at instruction %#018" PRIx64,
                             bad);
                }
                ok = 0;
                break;
            }
            finish_vertex(&tv[nv++], env->out, xf);
        }
        for (uint32_t i = 0; i < to.nindices && ok; i++) {
            tidx[ni++] = base + to.indices[i];
        }
        tess_free(&to);
    }
    env->in_vertices = NULL;
    env->num_in_vertices = 0;
    env->patch = NULL;
    free(tcs_out);
    free(patch);
    const vertex **prim = ok && ni ? malloc((size_t)ni * sizeof(*prim)) : NULL;
    if (!prim) {
        free(tv);
        free(tidx);
        return 0;
    }
    for (uint32_t k = 0; k < ni; k++) {
        prim[k] = &tv[tidx[k]];
    }
    free(tidx);
    *out_verts = tv;
    *out_prim = prim;
    *out_n = ni;
    *out_lines = lines;
    return 1;
}

/*
 * The rectangle of the line width around segment a-b: two triangles over copies
 * of the segment's ends, so attributes vary along the line only. No smoothing
 * (coverage alpha) yet. Returns 0 for a segment without length.
 */
static int line_rect(const uint32_t *r, const vertex *a, const vertex *b, vertex q[4],
                     frag_tri t[2]) {
    uint32_t gx, gy;
    gpu_ms_grid(r[M3D_MS_MODE], &gx, &gy);
    float hw = fbits(r[M3D_LINE_WIDTH_ALIASED]) * 0.5f;
    hw = hw > 0.5f ? hw : 0.5f;
    float dx = b->win[0] - a->win[0], dy = b->win[1] - a->win[1];
    float len = sqrtf(dx * dx + dy * dy);
    if (len == 0.0f) {
        return 0;
    }
    /* the width is in pixels; window coordinates are in samples */
    float nx = -dy / len * hw * (float)gx, ny = dx / len * hw * (float)gy;
    const vertex *ends[4] = {a, a, b, b};
    const float side[4] = {1, -1, -1, 1};
    for (int c = 0; c < 4; c++) {
        memcpy(&q[c], ends[c], sizeof(*q));
        q[c].win[0] += side[c] * nx;
        q[c].win[1] += side[c] * ny;
    }
    t[0] = (frag_tri){{&q[0], &q[1], &q[2]}, 1};
    t[1] = (frag_tri){{&q[0], &q[2], &q[3]}, 1};
    return 1;
}

static void draw_programs(const gpu_draw_state *s, uint32_t topology, const uint32_t *indices,
                          uint32_t first, uint32_t count, uint32_t instance, const programs *pg,
                          gpu_draw_stats *st) {
    const uint32_t *r = s->r;
    const program vs = pg->vs, fs = pg->fs;
    targets ts;
    ts.n = r[M3D_CT_SELECT] & 0xF ? r[M3D_CT_SELECT] & 0xF : 1;
    ts.n = ts.n > 8 ? 8 : ts.n;
    for (uint32_t k = 0; k < ts.n; k++) {
        if (!bind_target(r, k, &ts.t[k])) {
            HOS_LOGW(MOD, "draw: render target %u format %#x not supported or not mapped", k,
                     ts.t[k].format);
            if (k == 0) {
                return;
            }
        }
    }
    if (count == 0 || count > MAX_VERTICES) {
        return;
    }
    depth_target dt;
    bind_depth(r, &dt);
    vertex *verts = calloc(count, sizeof(*verts));  /* shaded vertices, at most one per index */
    const vertex **vp = calloc(count, sizeof(*vp)); /* per index: its shaded vertex */
    shader_env *env = calloc(1, sizeof(*env));
    if (!verts || !vp || !env) {
        free(verts);
        free(vp);
        free(env);
        return;
    }
    /* Post-transform reuse for indexed draws: an index seen before reuses its vertex,
     * when the index range is small enough for a direct map. */
    int32_t *reuse = NULL;
    uint32_t vmin = 0;
    if (indices) {
        uint32_t vmax = 0;
        vmin = UINT32_MAX;
        for (uint32_t k = 0; k < count; k++) {
            vmin = indices[k] < vmin ? indices[k] : vmin;
            vmax = indices[k] > vmax ? indices[k] : vmax;
        }
        uint64_t range = (uint64_t)vmax - vmin + 1;
        reuse = range <= 4ull * count ? malloc(range * sizeof(*reuse)) : NULL;
        if (reuse) {
            memset(reuse, 0xFF, range * sizeof(*reuse));
        }
    }

    /* Vertex stage. */
    bind_cbufs(s, 0, env);
    uint32_t gx, gy;
    gpu_ms_grid(r[M3D_MS_MODE], &gx, &gy);
    const xform xf = {.sx = fbits(r[M3D_VIEWPORT + 0]) * (float)gx,
                      .sy = fbits(r[M3D_VIEWPORT + 1]) * (float)gy,
                      .sz = fbits(r[M3D_VIEWPORT + 2]),
                      .tx = fbits(r[M3D_VIEWPORT + 3]) * (float)gx,
                      .ty = fbits(r[M3D_VIEWPORT + 4]) * (float)gy,
                      .tz = fbits(r[M3D_VIEWPORT + 5])};
    uint32_t nv = 0;
    for (uint32_t k = 0; k < count; k++) {
        if (reuse && reuse[indices[k] - vmin] >= 0) {
            vp[k] = &verts[reuse[indices[k] - vmin]];
            continue;
        }
        memset(env->in, 0, sizeof(env->in));
        memset(env->out, 0, sizeof(env->out));
        uint32_t vid = indices ? indices[k] : first + k;
        env->in[0x2fc / 4] = vid;
        env->in[0x2f8 / 4] = instance;
        if (!fetch_attribs(r, vid, instance, env)) {
            HOS_LOGW(MOD, "vertex %u: attribute data not mapped", vid);
        }
        uint64_t bad = 0;
        if (shader_exec(vs.prog, env, &bad) != 0) {
            if (!st->shader_errors++) {
                HOS_LOGW(MOD, "vertex shader stopped at instruction %#018" PRIx64, bad);
            }
            free(reuse);
            free(vp);
            free(verts);
            free(env);
            return;
        }
        if (reuse) {
            reuse[indices[k] - vmin] = (int32_t)nv;
        }
        vertex *v = &verts[nv++];
        vp[k] = v;
        finish_vertex(v, env->out, &xf);
        if (st->draws <= 1 && k < 4) {
            HOS_LOGT(MOD, "vertex %u: clip (%g %g %g %g) window (%g %g %g) out[0x80] (%g %g %g %g)",
                     first + k, v->pos[0], v->pos[1], v->pos[2], v->pos[3], v->win[0], v->win[1],
                     v->win[2], fbits(v->out[0x20]), fbits(v->out[0x21]), fbits(v->out[0x22]),
                     fbits(v->out[0x23]));
        }
    }
    free(reuse);

    /* Tessellation: patches become the generated triangles (or isoline segments). */
    vertex *tverts = NULL;
    const vertex **prim = vp;
    uint32_t nprim = count;
    if (topology == TOPO_PATCHES) {
        const vertex **tprim = NULL;
        int lines = 0;
        nprim = 0;
        if (!pg->tes.prog) {
            HOS_LOGW(MOD, "patches without a tessellation evaluation shader");
        } else if (tessellate(s, pg, vp, count, env, &xf, &tverts, &tprim, &nprim, &lines, st)) {
            prim = tprim;
            topology = lines ? TOPO_LINES : TOPO_TRIANGLES;
        }
    }

    /* Primitive assembly: filled triangles into `tris`, line segments (and the
     * triangles the polygon mode draws as lines, culled as triangles first) as pairs
     * of ends in `segs`, then rectangles over `lv` for the segments. */
    int line = topology == TOPO_LINES || topology == TOPO_LINE_LOOP || topology == TOPO_LINE_STRIP;
    uint32_t nseg = topology == TOPO_LINES        ? nprim / 2
                    : topology == TOPO_LINE_STRIP ? (nprim ? nprim - 1 : 0)
                    : topology == TOPO_LINE_LOOP  ? (nprim > 1 ? nprim : 0)
                                                  : 0;
    uint32_t cap = line ? 1 : topology == TOPO_QUADS ? nprim / 4 * 2 : nprim;
    frag_tri *tris = calloc(cap ? cap : 1, sizeof(*tris));
    const vertex **segs = line && nseg ? calloc(2 * (size_t)nseg, sizeof(*segs)) : NULL;
    vertex *lv = NULL;
    uint32_t nt = 0, ns = 0;
    switch (tris && (!line || !nseg || segs) ? topology : ~0u) {
    case TOPO_LINES:
    case TOPO_LINE_LOOP:
    case TOPO_LINE_STRIP:
        for (uint32_t k = 0; k < nseg; k++) {
            segs[2 * k] = topology == TOPO_LINES ? prim[2 * k] : prim[k];
            segs[2 * k + 1] = topology == TOPO_LINES ? prim[2 * k + 1] : prim[(k + 1) % nprim];
        }
        ns = nseg;
        break;
    case TOPO_TRIANGLES:
        for (uint32_t k = 0; k + 2 < nprim; k += 3) {
            tris[nt++] = (frag_tri){{prim[k], prim[k + 1], prim[k + 2]}, 0};
        }
        break;
    case TOPO_STRIP:
        for (uint32_t k = 0; k + 2 < nprim; k++) {
            tris[nt++] = k & 1 ? (frag_tri){{prim[k + 1], prim[k], prim[k + 2]}, 0}
                               : (frag_tri){{prim[k], prim[k + 1], prim[k + 2]}, 0};
        }
        break;
    case TOPO_FAN:
        for (uint32_t k = 1; k + 1 < nprim; k++) {
            tris[nt++] = (frag_tri){{prim[0], prim[k], prim[k + 1]}, 0};
        }
        break;
    case TOPO_QUADS: /* each quad as two triangles sharing its first vertex */
        for (uint32_t k = 0; k + 3 < nprim; k += 4) {
            tris[nt++] = (frag_tri){{prim[k], prim[k + 1], prim[k + 2]}, 0};
            tris[nt++] = (frag_tri){{prim[k], prim[k + 2], prim[k + 3]}, 0};
        }
        break;
    case TOPO_PATCHES: /* nothing generated */
        break;
    default:
        HOS_LOGW(MOD, "topology %u not supported yet", topology);
        break;
    }
    /* Polygon mode line (0x1B01, by facing): a triangle's three edges instead of
     * its area. Point mode (0x1B00) still fills. Unlike the hardware, an edge two
     * triangles share is drawn twice (and blended twice), a triangle without area
     * loses its edges, and the polygon offset for lines is not applied. */
    if (nt && (r[M3D_POLYGON_MODE_FRONT] == 0x1B01 || r[M3D_POLYGON_MODE_BACK] == 0x1B01)) {
        const vertex **esegs = calloc(6 * (size_t)nt, sizeof(*esegs));
        if (esegs) {
            uint32_t kept = 0;
            for (uint32_t k = 0; k < nt; k++) {
                const vertex *const *v = tris[k].v;
                int front = tri_front(r, v[0], v[1], v[2]);
                if (front < 0 || culled(r, front)) {
                    continue;
                }
                if (r[front ? M3D_POLYGON_MODE_FRONT : M3D_POLYGON_MODE_BACK] != 0x1B01) {
                    tris[kept++] = tris[k];
                    continue;
                }
                for (int e = 0; e < 3; e++) {
                    esegs[2 * ns] = v[e];
                    esegs[2 * ns + 1] = v[(e + 1) % 3];
                    ns++;
                }
            }
            nt = kept;
            free(segs);
            segs = esegs;
        }
    }
    /* The fragment stage. */
    bind_cbufs(s, 4, env);
    tex_ctx *tc = calloc(1, sizeof(*tc));
    if (tc) {
        /* Texture handles live in the stage's texture constant buffer (0x982, deko3d: c0);
         * deko3d binds its driver buffer explicitly only for the vertex stage, so fall back
         * to that binding and then to the buffer currently selected for updates. */
        uint32_t slot = r[M3D_TEX_CB_INDEX] < GPU_CBUFS ? r[M3D_TEX_CB_INDEX] : 0;
        uint64_t addr = s->cb_addr[4][slot];
        uint32_t size = s->cb_size[4][slot];
        if (!size) {
            addr = s->cb_addr[0][slot];
            size = s->cb_size[0][slot];
        }
        if (!size) {
            addr = ((uint64_t)(r[M3D_CB_ADDR_HI] & 0xFF) << 32) | r[M3D_CB_ADDR_LO];
            size = r[M3D_CB_SIZE];
        }
        tc->r = r;
        tc->handles = size ? nv_gpu_va_to_host(addr, size) : NULL;
        tc->handles_size = tc->handles ? size : 0;
        env->tex = tex_sample;
        env->texel = tex_texel;
        env->tex_user = tc;
    }
    frag_job *job = calloc(1, sizeof(*job));
    if (job) {
        *job = (frag_job){.r = r, .fs = &fs, .env = env, .tc = tc, .ts = &ts, .dt = &dt};
        if (nt) {
            run_frag_job(job, tris, nt, st);
        }
        /* Lines in chunks: each needs four vertex copies, and a tessellated draw in
         * line polygon mode can have hundreds of thousands of them. */
        lv = ns ? calloc(4 * (size_t)LINE_CHUNK, sizeof(*lv)) : NULL;
        frag_tri *ltris = ns ? calloc(2 * (size_t)LINE_CHUNK, sizeof(*ltris)) : NULL;
        for (uint32_t k0 = 0; k0 < ns && lv && ltris; k0 += LINE_CHUNK) {
            uint32_t n = ns - k0 < LINE_CHUNK ? ns - k0 : LINE_CHUNK, lt = 0;
            for (uint32_t k = 0; k < n; k++) {
                lt += 2 * line_rect(r, segs[2 * (k0 + k)], segs[2 * (k0 + k) + 1], &lv[4 * k],
                                    &ltris[lt]);
            }
            if (lt) {
                run_frag_job(job, ltris, lt, st);
            }
        }
        free(ltris);
        free(job);
    }
    free(segs);
    free(tris);
    free(lv);
    free(tc);
    if (prim != vp) {
        free(prim);
    }
    free(tverts);
    free(vp);
    free(verts);
    free(env);
}

/*
 * 2D engine (0x902D) scaled blit: destination surface 0x80-0x89, source surface
 * 0x8C-0x95 (format, linear, block size, depth, layer, pitch, width, height,
 * address hi, lo), sample mode 0x223 ([4] bilinear), destination rectangle
 * 0x22C-0x22F, steps du/dx and dv/dy as 32.32 fixed point at 0x230-0x233, and
 * the source origin likewise at 0x234-0x237, whose last word starts the blit.
 */
typedef struct surface_2d {
    uint8_t *base;
    uint64_t size;
    uint32_t format, bpp, width, height, row_bytes, bh;
    int pitch_layout;
} surface_2d;

static int bind_2d(const uint32_t *r, uint32_t base, surface_2d *sf) {
    uint8_t probe[16];
    float zero[4] = {0, 0, 0, 0};
    sf->format = r[base + 0];
    sf->bpp = gpu_encode_color(sf->format, zero, probe);
    sf->pitch_layout = r[base + 1] & 1;
    sf->bh = (r[base + 2] >> 4) & 0xF;
    sf->width = r[base + 6];
    sf->height = r[base + 7];
    sf->row_bytes = sf->pitch_layout ? r[base + 5] : sf->width * sf->bpp;
    if (!sf->bpp || !sf->width || !sf->height) {
        return 0;
    }
    sf->size = sf->pitch_layout ? (uint64_t)sf->row_bytes * sf->height
                                : gpu_bl_size(sf->row_bytes, sf->height, sf->bh);
    uint64_t va = ((uint64_t)(r[base + 8] & 0xFF) << 32) | r[base + 9];
    sf->base = nv_gpu_va_to_host(va, sf->size);
    return sf->base != NULL;
}

static uint8_t *px_2d(const surface_2d *sf, int32_t x, int32_t y) {
    x = x < 0 ? 0 : x >= (int32_t)sf->width ? (int32_t)sf->width - 1 : x;
    y = y < 0 ? 0 : y >= (int32_t)sf->height ? (int32_t)sf->height - 1 : y;
    uint64_t off = sf->pitch_layout
                       ? (uint64_t)y * sf->row_bytes + (uint64_t)x * sf->bpp
                       : gpu_bl_offset((uint32_t)x * sf->bpp, (uint32_t)y, sf->row_bytes, sf->bh);
    return off + sf->bpp <= sf->size ? sf->base + off : NULL;
}

typedef struct blit_job {
    surface_2d dst, src;
    double du, dv, x0, y0;
    int bilinear;
    uint32_t dx0, dy0, w, h;
} blit_job;

static void blit_rows(void *arg, uint32_t band, uint32_t nbands) {
    const blit_job *b = arg;
    for (uint32_t j = 0; j < b->h; j++) {
        if ((j / BAND_ROWS) % nbands != band) {
            continue;
        }
        for (uint32_t i = 0; i < b->w; i++) {
            /* x0/y0 is where the first destination pixel's centre samples the source */
            double sxp = b->x0 + (double)i * b->du, syp = b->y0 + (double)j * b->dv;
            float out[4] = {0, 0, 0, 0};
            if (b->bilinear) {
                /* centre of the destination pixel's footprint, in source texel space */
                double cx = sxp + (b->du - 1.0) / 2.0, cy = syp + (b->dv - 1.0) / 2.0;
                int32_t xi = (int32_t)floor(cx - 0.5), yi = (int32_t)floor(cy - 0.5);
                float ax = (float)(cx - 0.5 - xi), ay = (float)(cy - 0.5 - yi), t[4][4];
                for (int k = 0; k < 4; k++) {
                    uint8_t *p = px_2d(&b->src, xi + (k & 1), yi + (k >> 1));
                    memset(t[k], 0, sizeof(t[k]));
                    if (p) {
                        gpu_decode_color(b->src.format, p, t[k]);
                    }
                }
                for (int c = 0; c < 4; c++) {
                    float top = t[0][c] + (t[1][c] - t[0][c]) * ax;
                    float bot = t[2][c] + (t[3][c] - t[2][c]) * ax;
                    out[c] = top + (bot - top) * ay;
                }
            } else {
                uint8_t *p = px_2d(&b->src, (int32_t)floor(sxp), (int32_t)floor(syp));
                if (p) {
                    gpu_decode_color(b->src.format, p, out);
                }
            }
            uint8_t *d = px_2d(&b->dst, (int32_t)(b->dx0 + i), (int32_t)(b->dy0 + j));
            if (d) {
                uint8_t enc[16];
                gpu_encode_color(b->dst.format, out, enc);
                memcpy(d, enc, b->dst.bpp);
            }
        }
    }
}

void gpu_2d_blit(const uint32_t *r) {
    blit_job b;
    if (!bind_2d(r, 0x80, &b.dst) || !bind_2d(r, 0x8C, &b.src)) {
        HOS_LOGW(MOD, "2d blit: surface format %#x -> %#x not supported or not mapped", r[0x8C],
                 r[0x80]);
        return;
    }
    b.du = (double)r[0x231] + (double)r[0x230] / 4294967296.0;
    b.dv = (double)r[0x233] + (double)r[0x232] / 4294967296.0;
    b.x0 = (double)(int32_t)r[0x235] + (double)r[0x234] / 4294967296.0;
    b.y0 = (double)(int32_t)r[0x237] + (double)r[0x236] / 4294967296.0;
    b.bilinear = (r[0x223] >> 4) & 1;
    b.dx0 = r[0x22C];
    b.dy0 = r[0x22D];
    b.w = r[0x22E];
    b.h = r[0x22F];
    pool_run(blit_rows, &b);
    HOS_LOGD(MOD, "2d blit %ux%u -> %ux%u at (%u,%u), step %.2f %s", b.src.width, b.src.height, b.w,
             b.h, b.dx0, b.dy0, b.du, b.bilinear ? "bilinear" : "point");
}

/*
 * Compute (0xB1C0). deko3d uploads a queue meta data block (QMD, v01_07) with
 * inline-to-memory and launches it: 0x0AD holds its address >> 8 and 0x0AF
 * starts it. Fields used, by 32-bit word: 8 program offset (from the code
 * address at 0x582/0x583), 12 grid x, 13 grid y [15:0] and z [31:16], 18
 * threads x [31:16], 19 threads y [15:0] and z [31:16], 20 [7:0] valid constant
 * buffers, 29 + 2i address low and 30 + 2i address high [7:0] and size
 * [31:15] of buffer i. Threads run one after another, which is exact for
 * kernels without barriers or shared memory.
 */
static void *compute_mem(uint64_t va, uint32_t len) {
    return nv_gpu_va_to_host(va, len);
}

void gpu_compute_launch(const uint32_t *rc, gpu_draw_stats *st) {
    uint64_t qmd_va = (uint64_t)rc[0x0AD] << 8;
    const uint32_t *q = nv_gpu_va_to_host(qmd_va, 0x100);
    if (!q) {
        HOS_LOGW(MOD, "compute: QMD at %#" PRIx64 " not mapped", qmd_va);
        return;
    }
    uint64_t base = ((uint64_t)(rc[M3D_PROGRAM_HI] & 0xFF) << 32) | rc[M3D_PROGRAM_LO];
    uint64_t avail = 0;
    const uint64_t *code = nv_gpu_va_to_host_span(base + q[8], &avail);
    if (!code || avail < 16) {
        HOS_LOGW(MOD, "compute: program at %#" PRIx64 " not mapped", base + q[8]);
        return;
    }
    uint32_t words = (uint32_t)(avail / 8 < 65536 ? avail / 8 : 65536);
    uint32_t grid[3] = {q[12], q[13] & 0xFFFF, q[13] >> 16};
    uint32_t block[3] = {q[18] >> 16, q[19] & 0xFFFF, q[19] >> 16};
    shader_env *env = calloc(1, sizeof(*env));
    if (!env) {
        return;
    }
    for (uint32_t i = 0; i < GPU_CBUFS && i < 8; i++) {
        if ((q[20] >> i) & 1) {
            uint64_t va = ((uint64_t)(q[30 + 2 * i] & 0xFF) << 32) | q[29 + 2 * i];
            uint32_t size = q[30 + 2 * i] >> 15;
            env->cb[i] = nv_gpu_va_to_host(va, size);
            env->cb_size[i] = env->cb[i] ? size : 0;
        }
    }
    env->mem = compute_mem;
    shader_prog *prog = shader_prog_get(code, words);
    if (!prog) {
        free(env);
        return;
    }
    HOS_LOGD(MOD, "compute: %ux%ux%u groups of %ux%ux%u at %#" PRIx64, grid[0], grid[1], grid[2],
             block[0], block[1], block[2], base + q[8]);
    for (uint32_t c = 0; c < 3; c++) {
        env->ntid[c] = block[c];
        env->nctaid[c] = grid[c];
    }
    int stop = 0;
    for (uint32_t gz = 0; gz < grid[2] && !stop; gz++) {
        for (uint32_t gy_ = 0; gy_ < grid[1] && !stop; gy_++) {
            for (uint32_t gx_ = 0; gx_ < grid[0] && !stop; gx_++) {
                for (uint32_t t = 0; t < block[0] * block[1] * block[2]; t++) {
                    env->ctaid[0] = gx_;
                    env->ctaid[1] = gy_;
                    env->ctaid[2] = gz;
                    env->tid[0] = t % block[0];
                    env->tid[1] = t / block[0] % block[1];
                    env->tid[2] = t / (block[0] * block[1]);
                    uint64_t bad = 0;
                    if (shader_exec(prog, env, &bad) != 0) {
                        if (!st->shader_errors++) {
                            HOS_LOGW(MOD, "compute shader stopped at instruction %#018" PRIx64,
                                     bad);
                        }
                        stop = 1;
                        break;
                    }
                }
            }
        }
    }
    shader_prog_release(prog);
    st->dispatches++;
    free(env);
}

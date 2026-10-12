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
    M3D_PIPELINE = 0x800,      /* select, offset, ..., register count */
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
                            gpu_draw_stats *st) {
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
            write_pixel(t, (uint32_t)x, (uint32_t)y, rgba);
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
            raster_triangle(r, &verts[k], &verts[k + 1], &verts[k + 2], &fs, env, &t, st);
        }
        break;
    case TOPO_STRIP:
        for (uint32_t k = 0; k + 2 < count; k++) {
            if (k & 1) {
                raster_triangle(r, &verts[k + 1], &verts[k], &verts[k + 2], &fs, env, &t, st);
            } else {
                raster_triangle(r, &verts[k], &verts[k + 1], &verts[k + 2], &fs, env, &t, st);
            }
        }
        break;
    case TOPO_FAN:
        for (uint32_t k = 1; k + 1 < count; k++) {
            raster_triangle(r, &verts[0], &verts[k], &verts[k + 1], &fs, env, &t, st);
        }
        break;
    case TOPO_QUADS: /* each quad as two triangles sharing its first vertex */
        for (uint32_t k = 0; k + 3 < count; k += 4) {
            raster_triangle(r, &verts[k], &verts[k + 1], &verts[k + 2], &fs, env, &t, st);
            raster_triangle(r, &verts[k], &verts[k + 2], &verts[k + 3], &fs, env, &t, st);
        }
        break;
    default:
        HOS_LOGW(MOD, "topology %u not supported yet", topology);
        break;
    }
    free(verts);
    free(env);
}

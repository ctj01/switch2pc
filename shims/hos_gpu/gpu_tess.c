/*
 * Tessellation primitive generator; see gpu_tess.h. Triangles and quads are
 * built as the specification describes them: an outer ring whose edges carry
 * the outer levels, stitched to the inner subdivision (concentric triangles
 * for the triangle domain, a grid for quads), every ring wound the same way
 * so the stitching between two rings is a walk along both edges.
 */

#include "gpu_tess.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

uint32_t tess_segments(float level, uint32_t spacing) {
    if (!(level >= 1.0f)) { /* also NaN */
        level = 1.0f;
    }
    if (level > (float)TESS_MAX_LEVEL) {
        level = (float)TESS_MAX_LEVEL;
    }
    uint32_t n = (uint32_t)ceilf(level);
    if (spacing == TESS_FRACTIONAL_EVEN) {
        n = n < 2 ? 2 : n + (n & 1);
    } else if (spacing == TESS_FRACTIONAL_ODD) {
        n = n & 1 ? n : (n >= TESS_MAX_LEVEL ? TESS_MAX_LEVEL - 1 : n + 1);
    }
    return n;
}

/* Output buffers grow as primitives are appended. */
typedef struct builder {
    tess_output *o;
    uint32_t cap_coords, cap_indices;
    int failed;
} builder;

static uint32_t add_coord(builder *b, float u, float v, float w) {
    tess_output *o = b->o;
    if (o->ncoords == b->cap_coords) {
        uint32_t cap = b->cap_coords ? 2 * b->cap_coords : 64;
        tess_coord *c = realloc(o->coords, (size_t)cap * sizeof(*c));
        if (!c) {
            b->failed = 1;
            return 0;
        }
        o->coords = c;
        b->cap_coords = cap;
    }
    o->coords[o->ncoords] = (tess_coord){u, v, w};
    return o->ncoords++;
}

static void add_index(builder *b, uint32_t i) {
    tess_output *o = b->o;
    if (o->nindices == b->cap_indices) {
        uint32_t cap = b->cap_indices ? 2 * b->cap_indices : 192;
        uint32_t *p = realloc(o->indices, (size_t)cap * sizeof(*p));
        if (!p) {
            b->failed = 1;
            return;
        }
        o->indices = p;
        b->cap_indices = cap;
    }
    o->indices[o->nindices++] = i;
}

static void add_triangle(builder *b, int cw, uint32_t i0, uint32_t i1, uint32_t i2) {
    add_index(b, i0);
    add_index(b, cw ? i2 : i1);
    add_index(b, cw ? i1 : i2);
}

/*
 * Fill the band between an outer edge (points o[0..no]) and the inner edge facing
 * it (points in[0..ni], ni may be 0 for a single point), both running the same
 * way round their ring, with triangles that keep that orientation: advance along
 * whichever edge is behind in its own parameter.
 */
static void stitch(builder *b, int cw, const uint32_t *o, uint32_t no, const uint32_t *in,
                   uint32_t ni) {
    uint32_t i = 0, j = 0;
    while (i < no || j < ni) {
        if (j == ni || (i < no && (uint64_t)(i + 1) * ni <= (uint64_t)(j + 1) * no)) {
            add_triangle(b, cw, o[i], o[i + 1], in[j]);
            i++;
        } else {
            add_triangle(b, cw, in[j + 1], in[j], o[i]);
            j++;
        }
    }
}

/* A ring of the triangle domain. Ring k's edges are parallel to the patch's and
 * every segment, on every ring, is one n-th of an edge long (the inner corners
 * are where the perpendiculars through the first subdivision points meet), so
 * corner c of ring k sits at coordinate c = 1 - 4k/3n, the others 2k/3n. Edge e
 * runs from corner e+1 to corner e+2: edge 0 is u = 0, 1 is v = 0 and 2 is
 * w = 0, as the outer levels are numbered. Returns the ring's first index. */
static uint32_t tri_ring(builder *b, uint32_t k, uint32_t n, const uint32_t seg[3]) {
    uint32_t first = b->o->ncoords;
    if (seg[0] == 0) { /* the centre point */
        add_coord(b, 1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f);
        return first;
    }
    float lo = 2.0f * (float)k / (3.0f * (float)n), hi = 1.0f - 2.0f * lo;
    for (uint32_t e = 0; e < 3; e++) {
        for (uint32_t i = 0; i < seg[e]; i++) {
            /* along edge e coordinate e stays lo (exactly 0 on the patch's own edges),
             * e+1 falls from hi to lo and e+2 takes the rest */
            float t = (float)i / (float)seg[e], p[3];
            p[e] = lo;
            p[(e + 1) % 3] = hi + (lo - hi) * t;
            p[(e + 2) % 3] = 1.0f - lo - p[(e + 1) % 3];
            add_coord(b, p[0], p[1], p[2]);
        }
    }
    return first;
}

/* Point i of edge e on a ring of 3 edges with `seg` segments each, starting at `first`. */
static void ring_edge(uint32_t first, const uint32_t seg[3], uint32_t e, uint32_t *idx) {
    uint32_t total = seg[0] + seg[1] + seg[2], off = 0;
    for (uint32_t k = 0; k < e; k++) {
        off += seg[k];
    }
    for (uint32_t i = 0; i <= seg[e]; i++) {
        idx[i] = total ? first + (off + i) % total : first;
    }
}

static void gen_triangles(builder *b, uint32_t spacing, int cw, const float outer[4],
                          const float inner[2]) {
    uint32_t seg0[3], n = tess_segments(inner[0], spacing);
    for (uint32_t e = 0; e < 3; e++) {
        seg0[e] = tess_segments(outer[e], spacing);
    }
    /* An inner level of one with a finer edge still gets a centre: the level counts
     * as one plus epsilon (the specification's rule), so two segments. */
    if (n == 1 && (seg0[0] > 1 || seg0[1] > 1 || seg0[2] > 1)) {
        n = spacing == TESS_FRACTIONAL_ODD ? 3 : 2;
    }
    uint32_t rings = n / 2 + 1; /* ring k has n - 2k segments per edge; the last is 0 or 1 */
    uint32_t first_prev = tri_ring(b, 0, n, seg0), prev_seg[3] = {seg0[0], seg0[1], seg0[2]};
    uint32_t idx_o[TESS_MAX_LEVEL + 1], idx_i[TESS_MAX_LEVEL + 1];
    for (uint32_t k = 1; k < rings && !b->failed; k++) {
        uint32_t m = n - 2 * k, seg[3] = {m, m, m};
        uint32_t first = tri_ring(b, k, n, seg);
        for (uint32_t e = 0; e < 3; e++) {
            ring_edge(first_prev, prev_seg, e, idx_o);
            ring_edge(first, seg, e, idx_i);
            stitch(b, cw, idx_o, prev_seg[e], idx_i, m);
        }
        first_prev = first;
        memcpy(prev_seg, seg, sizeof(seg));
    }
    if (n & 1) { /* the innermost ring is one triangle */
        add_triangle(b, cw, first_prev, first_prev + prev_seg[0],
                     first_prev + prev_seg[0] + prev_seg[1]);
    }
}

/* Quads: the outer ring runs (0,0) -> (1,0) -> (1,1) -> (0,1), its edges carrying
 * outer levels 1 (v = 0), 2 (u = 1), 3 (v = 1) and 0 (u = 0); inside, a grid of
 * nu x nv cells whose border is the inner ring. */
static uint32_t grid_index(uint32_t first, uint32_t nu, uint32_t i, uint32_t j) {
    (void)nu;
    return first + (j - 1) * (nu - 1) + (i - 1);
}

static void gen_quads(builder *b, uint32_t spacing, int cw, const float outer[4],
                      const float inner[2]) {
    uint32_t so[4], nu = tess_segments(inner[0], spacing), nv = tess_segments(inner[1], spacing);
    for (uint32_t e = 0; e < 4; e++) {
        so[e] = tess_segments(outer[e], spacing);
    }
    /* One quad only when all six levels are one; otherwise an inner level of one
     * counts as one plus epsilon, so the edges stay cut as their outer levels say. */
    int all_one = nu == 1 && nv == 1 && so[0] == 1 && so[1] == 1 && so[2] == 1 && so[3] == 1;
    if (nu == 1 && !all_one) {
        nu = spacing == TESS_FRACTIONAL_ODD ? 3 : 2;
    }
    if (nv == 1 && !all_one) {
        nv = spacing == TESS_FRACTIONAL_ODD ? 3 : 2;
    }
    if (all_one) { /* the patch as one cell */
        uint32_t first = b->o->ncoords;
        for (uint32_t j = 0; j <= nv; j++) {
            for (uint32_t i = 0; i <= nu; i++) {
                add_coord(b, (float)i / (float)nu, (float)j / (float)nv, 0.0f);
            }
        }
        for (uint32_t j = 0; j < nv; j++) {
            for (uint32_t i = 0; i < nu; i++) {
                uint32_t p = first + j * (nu + 1) + i;
                add_triangle(b, cw, p, p + 1, p + nu + 2);
                add_triangle(b, cw, p, p + nu + 2, p + nu + 1);
            }
        }
        return;
    }
    /* outer ring: edge order bottom, right, top, left */
    uint32_t first_o = b->o->ncoords;
    const uint32_t order[4] = {1, 2, 3, 0};
    const float from[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    const float to[4][2] = {{1, 0}, {1, 1}, {0, 1}, {0, 0}};
    uint32_t seg[4], total = 0;
    for (uint32_t s = 0; s < 4; s++) {
        seg[s] = so[order[s]];
        total += seg[s];
        for (uint32_t i = 0; i < seg[s]; i++) {
            float t = (float)i / (float)seg[s];
            add_coord(b, from[s][0] + (to[s][0] - from[s][0]) * t,
                      from[s][1] + (to[s][1] - from[s][1]) * t, 0.0f);
        }
    }
    /* inner grid points (i/nu, j/nv), 1 <= i < nu, 1 <= j < nv */
    uint32_t first_g = b->o->ncoords;
    for (uint32_t j = 1; j < nv; j++) {
        for (uint32_t i = 1; i < nu; i++) {
            add_coord(b, (float)i / (float)nu, (float)j / (float)nv, 0.0f);
        }
    }
    uint32_t idx_o[TESS_MAX_LEVEL + 1], idx_i[TESS_MAX_LEVEL + 1], off = 0;
    for (uint32_t s = 0; s < 4 && !b->failed; s++) {
        for (uint32_t i = 0; i <= seg[s]; i++) {
            idx_o[i] = first_o + (off + i) % total;
        }
        off += seg[s];
        uint32_t m = (s & 1) ? nv - 2 : nu - 2;
        for (uint32_t i = 0; i <= m; i++) {
            /* the inner side facing outer side s, same direction round the ring */
            uint32_t gi, gj;
            switch (s) {
            case 0:
                gi = 1 + i;
                gj = 1;
                break;
            case 1:
                gi = nu - 1;
                gj = 1 + i;
                break;
            case 2:
                gi = nu - 1 - i;
                gj = nv - 1;
                break;
            default:
                gi = 1;
                gj = nv - 1 - i;
                break;
            }
            idx_i[i] = grid_index(first_g, nu, gi, gj);
        }
        stitch(b, cw, idx_o, seg[s], idx_i, m);
    }
    for (uint32_t j = 1; j + 1 < nv; j++) {
        for (uint32_t i = 1; i + 1 < nu; i++) {
            uint32_t p00 = grid_index(first_g, nu, i, j), p10 = grid_index(first_g, nu, i + 1, j);
            uint32_t p01 = grid_index(first_g, nu, i, j + 1);
            uint32_t p11 = grid_index(first_g, nu, i + 1, j + 1);
            add_triangle(b, cw, p00, p10, p11);
            add_triangle(b, cw, p00, p11, p01);
        }
    }
}

/* Isolines: outer level 0 counts the lines (v = j / n0, the one at v = 1 is not
 * drawn), outer level 1 the segments along each (u = i / n1). */
static void gen_isolines(builder *b, uint32_t spacing, const float outer[4]) {
    uint32_t n0 = tess_segments(outer[0], TESS_EQUAL), n1 = tess_segments(outer[1], spacing);
    for (uint32_t j = 0; j < n0; j++) {
        uint32_t first = b->o->ncoords;
        for (uint32_t i = 0; i <= n1; i++) {
            add_coord(b, (float)i / (float)n1, (float)j / (float)n0, 0.0f);
        }
        for (uint32_t i = 0; i < n1; i++) {
            add_index(b, first + i);
            add_index(b, first + i + 1);
        }
    }
    b->o->lines = 1;
}

int tess_generate(uint32_t domain, uint32_t spacing, int cw, const float outer[4],
                  const float inner[2], tess_output *out) {
    memset(out, 0, sizeof(*out));
    uint32_t used = domain == TESS_TRIANGLES ? 3 : domain == TESS_QUADS ? 4 : 2;
    if (domain > TESS_QUADS) {
        return -1;
    }
    for (uint32_t e = 0; e < used; e++) {
        if (!(outer[e] > 0.0f)) {
            return 0; /* discarded */
        }
    }
    builder b = {.o = out};
    if (domain == TESS_TRIANGLES) {
        gen_triangles(&b, spacing, cw, outer, inner);
    } else if (domain == TESS_QUADS) {
        gen_quads(&b, spacing, cw, outer, inner);
    } else {
        gen_isolines(&b, spacing, outer);
    }
    if (b.failed) {
        tess_free(out);
        return -1;
    }
    return 0;
}

void tess_free(tess_output *out) {
    free(out->coords);
    free(out->indices);
    memset(out, 0, sizeof(*out));
}

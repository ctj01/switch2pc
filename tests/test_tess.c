/*
 * The tessellation primitive generator against the rules of OpenGL 4.6
 * section 11.2.2: segment counts per spacing mode, outer edges cut by their
 * own level, a consistent winding, and primitives that tile the domain
 * exactly once (their signed areas add up to the domain's).
 */
#include <math.h>
#include <string.h>

#include "gpu_tess.h"
#include "test.h"

static float signed_area(const tess_output *o, uint32_t t) {
    const tess_coord *a = &o->coords[o->indices[3 * t]], *b = &o->coords[o->indices[3 * t + 1]],
                     *c = &o->coords[o->indices[3 * t + 2]];
    return 0.5f * ((b->u - a->u) * (c->v - a->v) - (c->u - a->u) * (b->v - a->v));
}

/* Every triangle wound the same way, areas summing to `expect`; returns the count. */
static int check_tiling(const tess_output *o, float expect, int cw) {
    double sum = 0.0;
    for (uint32_t t = 0; t < o->nindices / 3; t++) {
        float a = signed_area(o, t);
        if (cw ? a >= 0.0f : a <= 0.0f) {
            return -1;
        }
        sum += a;
    }
    return fabs(fabs(sum) - expect) < 1e-5 ? (int)(o->nindices / 3) : -2;
}

/* Points of a triangle patch on the edge where coordinate `c` is 0. */
static uint32_t edge_points(const tess_output *o, int c) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < o->ncoords; i++) {
        const float *p = &o->coords[i].u;
        n += p[c] == 0.0f;
    }
    return n;
}

TEST(tess_segment_counts) {
    ASSERT_EQ(tess_segments(0.3f, TESS_EQUAL), 1);
    ASSERT_EQ(tess_segments(1.0f, TESS_EQUAL), 1);
    ASSERT_EQ(tess_segments(2.5f, TESS_EQUAL), 3);
    ASSERT_EQ(tess_segments(64.0f, TESS_EQUAL), 64);
    ASSERT_EQ(tess_segments(70.0f, TESS_EQUAL), 64);
    ASSERT_EQ(tess_segments(NAN, TESS_EQUAL), 1);
    ASSERT_EQ(tess_segments(1.0f, TESS_FRACTIONAL_EVEN), 2);
    ASSERT_EQ(tess_segments(3.0f, TESS_FRACTIONAL_EVEN), 4);
    ASSERT_EQ(tess_segments(4.0f, TESS_FRACTIONAL_EVEN), 4);
    ASSERT_EQ(tess_segments(1.0f, TESS_FRACTIONAL_ODD), 1);
    ASSERT_EQ(tess_segments(2.0f, TESS_FRACTIONAL_ODD), 3);
    ASSERT_EQ(tess_segments(64.0f, TESS_FRACTIONAL_ODD), 63);
}

TEST(tess_triangle_all_levels_one_is_the_patch) {
    const float outer[4] = {1, 1, 1, 0}, inner[2] = {1, 0};
    tess_output o;
    ASSERT_EQ(tess_generate(TESS_TRIANGLES, TESS_EQUAL, 0, outer, inner, &o), 0);
    ASSERT_EQ(o.ncoords, 3);
    ASSERT_EQ(o.nindices, 3);
    ASSERT_EQ(check_tiling(&o, 0.5f, 0), 1);
    for (uint32_t i = 0; i < 3; i++) {
        const tess_coord *c = &o.coords[i];
        ASSERT_TRUE(c->u + c->v + c->w == 1.0f);
        ASSERT_TRUE((c->u == 1.0f) + (c->v == 1.0f) + (c->w == 1.0f) == 1);
    }
    tess_free(&o);
}

TEST(tess_triangle_example_levels) {
    /* deko_examples 05: inner 5, outer 2, 3, 5: rings of 5, 3 and 1 segments */
    const float outer[4] = {2, 3, 5, 0}, inner[2] = {5, 0};
    tess_output o;
    ASSERT_EQ(tess_generate(TESS_TRIANGLES, TESS_EQUAL, 0, outer, inner, &o), 0);
    ASSERT_EQ(o.ncoords, 10u + 9u + 3u);
    ASSERT_TRUE(check_tiling(&o, 0.5f, 0) > 0);
    ASSERT_EQ(edge_points(&o, 0), 3); /* u = 0: outer level 0 */
    ASSERT_EQ(edge_points(&o, 1), 4); /* v = 0 */
    ASSERT_EQ(edge_points(&o, 2), 6); /* w = 0 */
    for (uint32_t i = 0; i < o.ncoords; i++) {
        const tess_coord *c = &o.coords[i];
        ASSERT_TRUE(c->u >= 0.0f && c->v >= 0.0f && c->w >= 0.0f);
        ASSERT_TRUE(fabsf(c->u + c->v + c->w - 1.0f) < 1e-6f);
    }
    /* the v = 0 edge is cut in thirds */
    int thirds = 0;
    for (uint32_t i = 0; i < o.ncoords; i++) {
        const tess_coord *c = &o.coords[i];
        thirds += c->v == 0.0f &&
                  (fabsf(c->u - 1.0f / 3.0f) < 1e-6f || fabsf(c->u - 2.0f / 3.0f) < 1e-6f);
    }
    ASSERT_EQ(thirds, 2);
    /* every index is in range and every coordinate is used */
    uint8_t used[64] = {0};
    for (uint32_t i = 0; i < o.nindices; i++) {
        ASSERT_TRUE(o.indices[i] < o.ncoords);
        used[o.indices[i]] = 1;
    }
    for (uint32_t i = 0; i < o.ncoords; i++) {
        ASSERT_EQ(used[i], 1);
    }
    tess_free(&o);
    /* clockwise: the same tiling, every triangle the other way round */
    ASSERT_EQ(tess_generate(TESS_TRIANGLES, TESS_EQUAL, 1, outer, inner, &o), 0);
    ASSERT_TRUE(check_tiling(&o, 0.5f, 1) > 0);
    tess_free(&o);
}

TEST(tess_triangle_inner_one_with_finer_edges_gets_a_centre) {
    const float outer[4] = {4, 1, 2, 0}, inner[2] = {1, 0};
    tess_output o;
    ASSERT_EQ(tess_generate(TESS_TRIANGLES, TESS_EQUAL, 0, outer, inner, &o), 0);
    ASSERT_EQ(o.ncoords, 7u + 1u);
    ASSERT_EQ(o.nindices / 3, 7);
    ASSERT_EQ(check_tiling(&o, 0.5f, 0), 7);
    const tess_coord *centre = &o.coords[7];
    ASSERT_TRUE(fabsf(centre->u - 1.0f / 3.0f) < 1e-6f && fabsf(centre->v - 1.0f / 3.0f) < 1e-6f);
    tess_free(&o);
}

TEST(tess_triangle_even_inner_level_ends_in_a_point) {
    const float outer[4] = {4, 4, 4, 0}, inner[2] = {4, 0};
    tess_output o;
    ASSERT_EQ(tess_generate(TESS_TRIANGLES, TESS_EQUAL, 0, outer, inner, &o), 0);
    ASSERT_EQ(o.ncoords, 12u + 6u + 1u);
    ASSERT_TRUE(check_tiling(&o, 0.5f, 0) > 0);
    tess_free(&o);
    /* fractional-odd rounds the counts up to 5, 5, 5 / 5 */
    ASSERT_EQ(tess_generate(TESS_TRIANGLES, TESS_FRACTIONAL_ODD, 0, outer, inner, &o), 0);
    ASSERT_EQ(o.ncoords, 15u + 9u + 3u);
    ASSERT_TRUE(check_tiling(&o, 0.5f, 0) > 0);
    tess_free(&o);
}

TEST(tess_discards_a_patch_with_a_non_positive_outer_level) {
    const float outer[4] = {2, 0, 5, 0}, inner[2] = {5, 0};
    tess_output o;
    ASSERT_EQ(tess_generate(TESS_TRIANGLES, TESS_EQUAL, 0, outer, inner, &o), 0);
    ASSERT_EQ(o.ncoords, 0);
    ASSERT_EQ(o.nindices, 0);
    const float nan_outer[4] = {2, NAN, 5, 0};
    ASSERT_EQ(tess_generate(TESS_TRIANGLES, TESS_EQUAL, 0, nan_outer, inner, &o), 0);
    ASSERT_EQ(o.ncoords, 0);
    tess_free(&o);
    ASSERT_EQ(tess_generate(3, TESS_EQUAL, 0, outer, inner, &o), -1);
}

TEST(tess_quads) {
    const float ones[4] = {1, 1, 1, 1}, inner1[2] = {1, 1};
    tess_output o;
    ASSERT_EQ(tess_generate(TESS_QUADS, TESS_EQUAL, 0, ones, inner1, &o), 0);
    ASSERT_EQ(o.ncoords, 4);
    ASSERT_EQ(check_tiling(&o, 1.0f, 0), 2);
    tess_free(&o);
    const float outer[4] = {2, 3, 4, 5}, inner[2] = {4, 3};
    ASSERT_EQ(tess_generate(TESS_QUADS, TESS_EQUAL, 0, outer, inner, &o), 0);
    ASSERT_EQ(o.ncoords, 14u + 3u * 2u);
    ASSERT_TRUE(check_tiling(&o, 1.0f, 0) > 0);
    uint32_t left = 0, bottom = 0, right = 0, top = 0;
    for (uint32_t i = 0; i < o.ncoords; i++) {
        const tess_coord *c = &o.coords[i];
        ASSERT_TRUE(c->u >= 0.0f && c->u <= 1.0f && c->v >= 0.0f && c->v <= 1.0f && c->w == 0.0f);
        left += c->u == 0.0f;
        bottom += c->v == 0.0f;
        right += c->u == 1.0f;
        top += c->v == 1.0f;
    }
    ASSERT_EQ(left, 3);   /* outer 0: u = 0 */
    ASSERT_EQ(bottom, 4); /* outer 1: v = 0 */
    ASSERT_EQ(right, 5);  /* outer 2: u = 1 */
    ASSERT_EQ(top, 6);    /* outer 3: v = 1 */
    tess_free(&o);
    /* an inner level of one with any other level above one counts as one plus
     * epsilon, so edges with outer level one keep two points (no T-junctions) */
    const float inner41[2] = {4, 1};
    ASSERT_EQ(tess_generate(TESS_QUADS, TESS_EQUAL, 0, ones, inner41, &o), 0);
    ASSERT_TRUE(check_tiling(&o, 1.0f, 0) > 0);
    left = bottom = right = top = 0;
    for (uint32_t i = 0; i < o.ncoords; i++) {
        const tess_coord *c = &o.coords[i];
        left += c->u == 0.0f;
        bottom += c->v == 0.0f;
        right += c->u == 1.0f;
        top += c->v == 1.0f;
    }
    ASSERT_EQ(left, 2);
    ASSERT_EQ(bottom, 2);
    ASSERT_EQ(right, 2);
    ASSERT_EQ(top, 2);
    tess_free(&o);
    /* an inner level of two leaves a degenerate inner ring (a line) */
    const float inner2[2] = {2, 5};
    ASSERT_EQ(tess_generate(TESS_QUADS, TESS_EQUAL, 1, outer, inner2, &o), 0);
    ASSERT_TRUE(check_tiling(&o, 1.0f, 1) > 0);
    tess_free(&o);
}

TEST(tess_isolines) {
    const float outer[4] = {3, 4, 0, 0}, inner[2] = {0, 0};
    tess_output o;
    ASSERT_EQ(tess_generate(TESS_ISOLINES, TESS_EQUAL, 0, outer, inner, &o), 0);
    ASSERT_EQ(o.lines, 1);
    ASSERT_EQ(o.ncoords, 3u * 5u);
    ASSERT_EQ(o.nindices, 2u * 12u);
    for (uint32_t i = 0; i < o.ncoords; i++) {
        ASSERT_TRUE(o.coords[i].v < 1.0f); /* the line at v = 1 belongs to the next patch */
    }
    for (uint32_t s = 0; s < o.nindices / 2; s++) {
        const tess_coord *a = &o.coords[o.indices[2 * s]], *b = &o.coords[o.indices[2 * s + 1]];
        ASSERT_TRUE(a->v == b->v && fabsf(b->u - a->u - 0.25f) < 1e-6f);
    }
    tess_free(&o);
}

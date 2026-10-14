#ifndef HOS_GPU_TESS_H
#define HOS_GPU_TESS_H

#include <stdint.h>

/*
 * Tessellation primitive generator (M5): from the tessellation levels of a
 * patch to the vertices (tessellation coordinates) and primitives the
 * evaluation shader then runs on, following OpenGL 4.6 section 11.2.2. The
 * levels, domain and spacing come from the 3D engine's TessellationMode
 * (0x0c8) and the patch space the control shader writes.
 */

enum {
    TESS_ISOLINES = 0, /* TessellationMode [1:0] */
    TESS_TRIANGLES = 1,
    TESS_QUADS = 2,
};

enum {
    TESS_EQUAL = 0, /* TessellationMode [5:4] */
    TESS_FRACTIONAL_ODD = 1,
    TESS_FRACTIONAL_EVEN = 2,
};

#define TESS_MAX_LEVEL 64

typedef struct tess_coord {
    float u, v, w; /* triangles: barycentric, u + v + w = 1; quads and isolines: w = 0 */
} tess_coord;

typedef struct tess_output {
    tess_coord *coords;
    uint32_t ncoords;
    uint32_t *indices; /* triangles: 3 per primitive; isolines: 2 per segment */
    uint32_t nindices;
    int lines; /* the primitives are line segments (isolines) */
} tess_output;

/**
 * Segments an edge with tessellation level `level` is cut into under `spacing`
 * (TESS_*): the level rounded up and clamped to [1, 64], to the next even count
 * from 2 for fractional-even, to the next odd count for fractional-odd.
 * FIXME(#44): fractional modes place their segments evenly here, where the
 * hardware shrinks two of them as the level grows.
 */
uint32_t tess_segments(float level, uint32_t spacing);

/**
 * Generate the primitives of one patch. `outer` holds the four outer levels
 * (triangles use three, isolines two), `inner` the two inner levels (triangles
 * one). A patch whose relevant outer level is NaN or not positive is discarded:
 * success with no coordinates. Triangles are wound counter-clockwise in the
 * (u, v) plane, clockwise when `cw` is set.
 *
 * @return 0 on success (the output must be released with tess_free), -1 when
 *         out of memory or `domain` is unknown
 */
int tess_generate(uint32_t domain, uint32_t spacing, int cw, const float outer[4],
                  const float inner[2], tess_output *out);

void tess_free(tess_output *out);

#endif /* HOS_GPU_TESS_H */

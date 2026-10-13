#ifndef HOS_GPU_SHADER_H
#define HOS_GPU_SHADER_H

#include <stdint.h>

/*
 * Maxwell (GM20B, SM 5.3) shader interpreter for the CPU rasteriser (M3).
 * It runs the instructions deko3d's compiler emits for vertex and fragment
 * shaders; encodings follow envytools' gm107 disassembler tables. An
 * instruction it does not know stops the shader with an error naming it.
 *
 * Attribute space (bytes): 0x70 position (x y z w), 0x80 + 16 * i generic
 * attribute i, 0x2f8 instance id, 0x2fc vertex id. Fragment shaders read
 * their inputs through IPA, which calls back into the rasteriser, and leave
 * colour outputs in r0, r1, ...
 *
 * Tessellation stages see a primitive, not a vertex: an attribute load names
 * the input vertex in a register (`in_vertices`), `ld p`/`st p` address the
 * per-patch space (`patch`: tessellation levels at 0x0 outer, 0x10 inner),
 * and the evaluation shader reads its tessellation coordinate from the output
 * space at 0x2f0 (u) and 0x2f4 (v), where the draw path puts it.
 */

#define SHADER_ATTR_WORDS 256 /* attribute space 0x000-0x3ff */
#define SHADER_CBUFS      18

typedef struct shader_env shader_env;

/* A shader decoded once into handlers and operands (see shader_prog_get). */
typedef struct shader_prog shader_prog;

/* IPA: mode 0 pass (screen-linear), 1 perspective (attr/w, the shader multiplies by w),
 * 2 constant (provoking vertex). `addr` is the attribute byte address. */
typedef float (*shader_ipa_fn)(void *user, uint32_t addr, uint32_t mode);

/* How a texture instruction chooses its level of detail. */
enum {
    SHADER_LOD_IMPLICIT = 0, /* from the coordinate derivatives (TEX, TEXS) */
    SHADER_LOD_ZERO = 1,     /* .LZ: level 0 */
    SHADER_LOD_EXPLICIT = 2, /* .LL: `lod` is the level */
    SHADER_LOD_BIAS = 3,     /* .LB: `lod` is added to the implicit level */
};

/* A sample's coordinates. The derivatives are those of (u, v) across the 2x2
 * pixel quad the fragment belongs to, when the fragment ran in a group that
 * had its quad (has_grad); a lane that runs alone has none and an implicit
 * lod is 0. */
typedef struct shader_tex_coord {
    float u, v;
    float dudx, dvdx, dudy, dvdy;
    int has_grad;
    int lod_mode;
    float lod;
} shader_tex_coord;

/* Texture sample: `index` is the instruction's texture index (the handle sits at
 * word `index` of the stage's texture constant buffer); coordinates are normalised. */
typedef void (*shader_tex_fn)(void *user, uint32_t index, const shader_tex_coord *c, float out[4]);

/* Texel fetch (texelFetch): integer texel coordinates of `level`, same `index`. */
typedef void (*shader_texel_fn)(void *user, uint32_t index, int32_t x, int32_t y, uint32_t level,
                                float out[4]);

struct shader_env {
    uint32_t in[SHADER_ATTR_WORDS];
    uint32_t out[SHADER_ATTR_WORDS];
    const uint8_t *cb[SHADER_CBUFS];
    uint32_t cb_size[SHADER_CBUFS];
    shader_ipa_fn ipa;
    void *user;
    /* compute: S2R's thread and group ids and counts; global memory for LDG/STG */
    uint32_t tid[3], ctaid[3], ntid[3], nctaid[3];
    uint32_t carry; /* IADD .CC / .X */
    void *(*mem)(uint64_t va, uint32_t len);
    shader_tex_fn tex;
    shader_texel_fn texel; /* shares tex_user */
    void *tex_user;
    /* tessellation: input vertices by index, the patch space, this TCS invocation */
    const uint32_t *const *in_vertices;
    uint32_t num_in_vertices;
    uint32_t *patch;
    uint32_t invocation_id;
    uint32_t r[260]; /* r255 (RZ) reads as zero; writes to it land in r[256] */
    uint8_t p[9];    /* p7 (PT) is always true; writes to it land in p[8] */
    int killed;      /* fragment discarded */
};

/**
 * The decoded form of the shader at `code`, from a cache keyed by address and
 * checked against the words, so a program is decoded once and shared, read
 * only, by every thread that runs it. Release it after the draw. NULL when out
 * of memory.
 */
shader_prog *shader_prog_get(const uint64_t *code, uint32_t code_words);
void shader_prog_release(shader_prog *prog);

/** Run a decoded shader; results as shader_run. Safe from several threads at once. */
int shader_exec(const shader_prog *prog, shader_env *env, uint64_t *bad_insn);

/**
 * Decode and run a shader from its first instruction (just after the 0x50-byte
 * header): shader_prog_get + shader_exec, for one-off runs.
 *
 * @param code        Instruction words (control words included, every fourth)
 * @param code_words  How many 64-bit words are readable at code
 * @param bad_insn    On error, the instruction that stopped it
 * @return 0 on EXIT, -1 on an unknown instruction or a runaway shader
 */
int shader_run(const uint64_t *code, uint32_t code_words, shader_env *env, uint64_t *bad_insn);

/*
 * Fragments in groups: one dispatch per instruction for up to SHADER_LANES
 * fragments. Registers are r[reg][lane], predicates and the carry are lane
 * masks; IPA asks for a lane. Results equal running each lane with shader_exec.
 */
#define SHADER_LANES 8

typedef float (*shader_ipa_lane_fn)(void *user, uint32_t lane, uint32_t addr, uint32_t mode);

typedef struct shader_group {
    uint32_t r[260][SHADER_LANES]; /* as shader_env.r, per lane */
    uint32_t p[9];                 /* as shader_env.p, one bit per lane */
    uint32_t carry;
    shader_ipa_lane_fn ipa;
    void *user;
} shader_group;

/**
 * Run `prog` for the lanes set in `active`. `env` supplies constant buffers and
 * texture callbacks, and is the template for lanes that leave the group (a
 * divergent branch, or an instruction groups do not run). Results as shader_exec.
 */
int shader_exec_group(const shader_prog *prog, shader_group *g, uint32_t active,
                      const shader_env *env, uint64_t *bad_insn);

#endif /* HOS_GPU_SHADER_H */

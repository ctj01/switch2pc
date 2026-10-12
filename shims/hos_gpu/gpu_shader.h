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
 */

#define SHADER_ATTR_WORDS 256 /* attribute space 0x000-0x3ff */
#define SHADER_CBUFS      18

typedef struct shader_env shader_env;

/* Decode cache (instruction word -> handler). Not thread-safe: one per worker. */
#define SHADER_DCACHE 1024
typedef struct shader_dcache shader_dcache;
shader_dcache *shader_dcache_new(void);

/* IPA: mode 0 pass (screen-linear), 1 perspective (attr/w, the shader multiplies by w),
 * 2 constant (provoking vertex). `addr` is the attribute byte address. */
typedef float (*shader_ipa_fn)(void *user, uint32_t addr, uint32_t mode);

/* Texture sample: `index` is the instruction's texture index (the handle sits at
 * word `index` of the stage's texture constant buffer); coordinates are normalised. */
typedef void (*shader_tex_fn)(void *user, uint32_t index, float u, float v, float out[4]);

/* Texel fetch (texelFetch, level 0): integer texel coordinates, same `index`. */
typedef void (*shader_texel_fn)(void *user, uint32_t index, int32_t x, int32_t y, float out[4]);

struct shader_env {
    uint32_t in[SHADER_ATTR_WORDS];
    uint32_t out[SHADER_ATTR_WORDS];
    const uint8_t *cb[SHADER_CBUFS];
    uint32_t cb_size[SHADER_CBUFS];
    shader_ipa_fn ipa;
    void *user;
    shader_dcache *dcache; /* optional */
    /* compute: S2R's thread and group ids and counts; global memory for LDG/STG */
    uint32_t tid[3], ctaid[3], ntid[3], nctaid[3];
    uint32_t carry; /* IADD .CC / .X */
    void *(*mem)(uint64_t va, uint32_t len);
    shader_tex_fn tex;
    shader_texel_fn texel; /* shares tex_user */
    void *tex_user;
    uint32_t r[256]; /* r255 reads as zero */
    uint8_t p[8];    /* p7 is always true */
    int killed;      /* fragment discarded */
};

/**
 * Run a shader from its first instruction (just after the 0x50-byte header).
 *
 * @param code        Instruction words (control words included, every fourth)
 * @param code_words  How many 64-bit words are readable at code
 * @param bad_insn    On error, the instruction that stopped it
 * @return 0 on EXIT, -1 on an unknown instruction or a runaway shader
 */
int shader_run(const uint64_t *code, uint32_t code_words, shader_env *env, uint64_t *bad_insn);

#endif /* HOS_GPU_SHADER_H */

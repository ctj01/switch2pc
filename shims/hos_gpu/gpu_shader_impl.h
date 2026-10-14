#ifndef HOS_GPU_SHADER_IMPL_H
#define HOS_GPU_SHADER_IMPL_H

/*
 * Shared between the scalar interpreter (gpu_shader.c) and the one that runs a
 * group of fragments per instruction (gpu_shader_simd.c): field helpers,
 * comparisons and the decoded program. Not part of the shader API.
 */

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "gpu_shader.h"

#define RZ 255
#define PT 7
/* Calls are costly under qemu-user: every return is an indirect branch it must look up. */
#define INLINE    inline __attribute__((always_inline))
#define MAX_STEPS 1000000
#define CF_DEPTH  16 /* SSY, PBK and PCNT nesting */

typedef uint64_t u64;

static INLINE uint32_t bits(u64 i, unsigned lo, unsigned n) {
    return (uint32_t)((i >> lo) & ((n == 32) ? 0xFFFFFFFFull : ((1ull << n) - 1)));
}

static INLINE int is(u64 i, u64 value, u64 mask) {
    return (i & mask) == value;
}

static INLINE float f(uint32_t v) {
    float x;
    memcpy(&x, &v, 4);
    return x;
}

static INLINE uint32_t u(float x) {
    uint32_t v;
    memcpy(&v, &x, 4);
    return v;
}

/* Operand B of the reg (0x5c..), cbuf (0x4c..) and 20-bit immediate (0x38..) forms. */
enum { BK_REG, BK_CBUF, BK_IMM };

static INLINE uint32_t imm20_int(u64 i) {
    uint32_t v = bits(i, 20, 19);
    return bits(i, 56, 1) ? v | 0xFFF80000u : v;
}

static INLINE uint32_t imm20_float(u64 i) {
    return (bits(i, 20, 19) | (bits(i, 56, 1) << 19)) << 12;
}

static INLINE float fmod_neg_abs(float x, int neg, int abs_) {
    if (abs_) {
        x = fabsf(x);
    }
    return neg ? -x : x;
}

static INLINE float sat(float x, int on) {
    if (!on) {
        return x;
    }
    return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x != x ? 0.0f : x;
}

/* Arithmetic results as the hardware gives them: a NaN is the canonical 0x7FFFFFFF,
 * whatever NaN the host would propagate (hosts differ, even scalar against vector). */
static INLINE uint32_t fres(float x) {
    return x != x ? 0x7FFFFFFFu : u(x);
}

static INLINE int fcmp(uint32_t op, float a, float b) {
    int unordered = a != a || b != b;
    switch (op & 7) {
    case 0:
        return op & 8 ? unordered : 0; /* f / nan */
    case 1:
        return a < b || ((op & 8) && unordered); /* lt / ltu */
    case 2:
        return a == b || ((op & 8) && unordered);
    case 3:
        return a <= b || ((op & 8) && unordered);
    case 4:
        return a > b || ((op & 8) && unordered);
    case 5:
        return (a != b && !unordered) || ((op & 8) && unordered);
    case 6:
        return a >= b || ((op & 8) && unordered);
    default:
        return op & 8 ? 1 : !unordered; /* t / num */
    }
}

static INLINE int icmp(uint32_t op, uint32_t a, uint32_t b, int is_signed) {
    int lt = is_signed ? (int32_t)a < (int32_t)b : a < b;
    int eq = a == b;
    switch (op) {
    case 1:
        return lt;
    case 2:
        return eq;
    case 3:
        return lt || eq;
    case 4:
        return !lt && !eq;
    case 5:
        return !eq;
    case 6:
        return !lt;
    case 7:
        return 1;
    default:
        return 0;
    }
}

static INLINE int combine(uint32_t op, int a, int b) {
    switch (op) {
    case 1:
        return a || b;
    case 2:
        return a ^ b;
    default:
        return a && b;
    }
}

/*
 * Decoded programs. A shader is decoded once into one entry per word (control
 * words become OP_CTRL): its handler, guard, registers and operand B already
 * resolved to a register, a constant buffer word or an immediate in the form
 * the instruction wants. The interpreter then dispatches on a dense switch
 * with no field extraction for the common operands. Programs are cached by
 * address, checked against a copy of their words, and shared read-only by the
 * fragment workers; the decoded form is also what a compiler to native code
 * would start from.
 */
enum {
    OP_UNKNOWN,
    OP_EXIT,
    OP_BRA,
    OP_SSY,
    OP_SYNC,
    OP_NOP,
    OP_ATTR,
    OP_LDC,
    OP_IPA,
    OP_RRO,
    OP_F2I,
    OP_I2F,
    OP_MUFU,
    OP_MOV32I,
    OP_MOV,
    OP_FADD,
    OP_FMUL,
    OP_FFMA,
    OP_FADD32I,
    OP_FMUL32I,
    OP_FFMA32I,
    OP_SHL,
    OP_SHR,
    OP_IADD,
    OP_ISCADD,
    OP_LOP,
    OP_SEL,
    OP_FMNMX,
    OP_FSETP,
    OP_ISETP,
    OP_TEXS,
    OP_TLDS,
    OP_TEX,
    OP_S2R,
    OP_GMEM,
    OP_XMAD,
    OP_BFE,
    OP_ISBERD,
    OP_CTRL, /* a scheduling control word */
};
_Static_assert(OP_ISBERD == 37, "OP_* must follow classify()'s numbering");

/*
 * Texture instruction forms. TEXS [56:53] (envydis tabd000): which registers
 * hold the coordinates and where the lod comes from; TLDS [56:53] (tabd200)
 * likewise with an integer lod; TEX [39:37] (tabdeb8): 0 implicit, 1 lz,
 * 2 lb, 3 ll, 6 lba, 7 lla, coordinates always A and A+1, lod or bias in B.
 * Array (a2d) forms sample the layer's 2D image with the layer ignored; 3D
 * and cube forms are unsupported and stop the shader.
 */
enum tex_coords {
    TEXC_A = 0,    /* 1D: u = A */
    TEXC_A_B = 1,  /* u = A, v = B */
    TEXC_A_A1 = 2, /* u = A, v = A+1 (lod, offset or sample in B) */
    TEXC_A1_B = 3, /* array: layer = A, u = A+1, v = B */
    TEXC_UNSUPPORTED = 4,
};

typedef struct tex_form {
    uint8_t coords; /* enum tex_coords */
    uint8_t lod;    /* SHADER_LOD_* */
} tex_form;

static inline tex_form texs_form(uint32_t f) {
    static const tex_form t[16] = {
        [0] = {TEXC_A, SHADER_LOD_ZERO},
        [1] = {TEXC_A_B, SHADER_LOD_IMPLICIT},
        [2] = {TEXC_A_B, SHADER_LOD_ZERO},
        [3] = {TEXC_A_A1, SHADER_LOD_EXPLICIT},
        /* 4, 5, 6 and 9 compare depth (.DC): u = A, v = A+1 and the reference in B,
         * against a sampler compare mode not implemented; the shader stops. */
        [4] = {TEXC_UNSUPPORTED, 0},
        [5] = {TEXC_UNSUPPORTED, 0},
        [6] = {TEXC_UNSUPPORTED, 0},
        [7] = {TEXC_A1_B, SHADER_LOD_IMPLICIT},
        [8] = {TEXC_A1_B, SHADER_LOD_ZERO},
        [9] = {TEXC_UNSUPPORTED, 0},
        [10] = {TEXC_UNSUPPORTED, 0},
        [11] = {TEXC_UNSUPPORTED, 0},
        [12] = {TEXC_UNSUPPORTED, 0},
        [13] = {TEXC_UNSUPPORTED, 0},
        [14] = {TEXC_UNSUPPORTED, 0},
        [15] = {TEXC_UNSUPPORTED, 0},
    };
    return t[f & 15];
}

static inline tex_form tlds_form(uint32_t f) {
    static const tex_form t[16] = {
        [0] = {TEXC_A, SHADER_LOD_ZERO},
        [1] = {TEXC_A, SHADER_LOD_EXPLICIT},
        [2] = {TEXC_A_B, SHADER_LOD_ZERO},
        [3] = {TEXC_UNSUPPORTED, 0},
        [4] = {TEXC_A_A1, SHADER_LOD_ZERO},
        [5] = {TEXC_A_A1, SHADER_LOD_EXPLICIT},
        [6] = {TEXC_A_A1, SHADER_LOD_ZERO},
        [7] = {TEXC_UNSUPPORTED, 0},
        [8] = {TEXC_UNSUPPORTED, 0},
        [9] = {TEXC_UNSUPPORTED, 0},
        [10] = {TEXC_UNSUPPORTED, 0},
        [11] = {TEXC_UNSUPPORTED, 0},
        [12] = {TEXC_A_A1, SHADER_LOD_EXPLICIT},
        [13] = {TEXC_UNSUPPORTED, 0},
        [14] = {TEXC_UNSUPPORTED, 0},
        [15] = {TEXC_UNSUPPORTED, 0},
    };
    return t[f & 15];
}

static inline uint32_t tex_lod_mode(uint32_t f) {
    switch (f & 7) {
    case 1:
        return SHADER_LOD_ZERO;
    case 2:
    case 6:
        return SHADER_LOD_BIAS;
    case 3:
    case 7:
        return SHADER_LOD_EXPLICIT;
    default:
        return SHADER_LOD_IMPLICIT;
    }
}

/* Component selection shared by TEXS and TLDS: with dst2 = RZ one of r g b a rg ra ga
 * ba, else rgb rga rba gba rgba; 9 ends the list. */
static const uint8_t tex_one[8][2] = {{0, 9}, {1, 9}, {2, 9}, {3, 9},
                                      {0, 1}, {0, 3}, {1, 3}, {2, 3}};
static const uint8_t tex_two[5][4] = {
    {0, 1, 2, 9}, {0, 1, 3, 9}, {0, 2, 3, 9}, {1, 2, 3, 9}, {0, 1, 2, 3}};

typedef struct pinsn {
    u64 raw;
    uint32_t b; /* B immediate, or constant buffer byte offset */
    uint8_t op;
    uint8_t guard; /* predicate [2:0], negated [3]; PT (7) runs always */
    uint8_t d, ra, rc;
    uint8_t bk, rb, cbi; /* B: kind, register, constant buffer */
    uint8_t ck;          /* FFMA: C from the constant buffer (B is then register rc) */
} pinsn;

struct shader_prog {
    const uint64_t *code; /* cache key, with words */
    uint64_t *words;      /* copy of the decoded words */
    pinsn *ins;
    uint32_t n;
    int implicit_lod; /* samples with derivatives: fragments must run as quads */
    uint32_t refs;    /* under g_progs_lock */
    struct shader_prog *next;
};

/* SSY/SYNC, PBK/BRK and PCNT/CONT reconvergence targets. */
typedef struct shader_cf {
    uint32_t stack[3][CF_DEPTH];
    uint32_t n[3];
} shader_cf;

/* The scalar interpreter from `pc` with control-flow state `cf` (updated). */
int shader_exec_from(const shader_prog *prog, shader_env *e, uint32_t pc, shader_cf *cf,
                     uint64_t *bad_insn);

#endif /* HOS_GPU_SHADER_IMPL_H */

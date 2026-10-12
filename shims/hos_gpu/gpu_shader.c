/*
 * Maxwell shader interpreter; see gpu_shader.h. Field positions are those of
 * envytools' envydis/gm107.c (dst [7:0], A [15:8], B [27:20], C [46:39],
 * guard predicate [18:16] + not [19], constant buffer index [38:34] and word
 * offset [33:20], 20-bit immediates [38:20] with the sign or float top bit
 * at [56], 32-bit immediates [51:20]). Every fourth 64-bit word is a
 * scheduling control word and is skipped.
 */

#include "gpu_shader.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

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

static INLINE uint32_t reg(const shader_env *e, uint32_t n) {
    return n == RZ ? 0 : e->r[n];
}

static INLINE void set_reg(shader_env *e, uint32_t n, uint32_t v) {
    if (n != RZ) {
        e->r[n] = v;
    }
}

static INLINE int pred(const shader_env *e, uint32_t n) {
    return n == PT ? 1 : e->p[n & 7];
}

static INLINE void set_pred(shader_env *e, uint32_t n, int v) {
    if (n != PT) {
        e->p[n & 7] = (uint8_t)(v != 0);
    }
}

static INLINE uint32_t cbuf_word(const shader_env *e, uint32_t idx, uint32_t byte_off) {
    if (idx >= SHADER_CBUFS || !e->cb[idx] || byte_off + 4 > e->cb_size[idx]) {
        return 0;
    }
    uint32_t v;
    memcpy(&v, e->cb[idx] + byte_off, 4);
    return v;
}

/* Operand B of the reg (0x5c..), cbuf (0x4c..) and 20-bit immediate (0x38..) forms. */
enum { FORM_REG, FORM_CBUF, FORM_IMM };

static INLINE uint32_t imm20_int(u64 i) {
    uint32_t v = bits(i, 20, 19);
    return bits(i, 56, 1) ? v | 0xFFF80000u : v;
}

static INLINE uint32_t imm20_float(u64 i) {
    return (bits(i, 20, 19) | (bits(i, 56, 1) << 19)) << 12;
}

static INLINE uint32_t operand_b(const shader_env *e, u64 i, int form, int is_float) {
    switch (form) {
    case FORM_REG:
        return reg(e, bits(i, 20, 8));
    case FORM_CBUF:
        return cbuf_word(e, bits(i, 34, 5), bits(i, 20, 14) * 4);
    default:
        return is_float ? imm20_float(i) : imm20_int(i);
    }
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
 * Decoding: classify() maps an instruction word to the branch of shader_run
 * that executes it (1..n, 0 unknown). It depends only on the word, so each
 * fragment worker keeps a direct-mapped cache of it that never needs flushing
 * (thread-local storage would cost more than it saves in a shared object);
 * without it every instruction of every pixel walked the whole chain.
 */
static uint8_t classify(u64 i) {
    if (is(i, 0xe300000000000000ull, 0xfff0000000000000ull)) {
        return 1;
    }
    if (is(i, 0xe240000000000000ull, 0xfff0000000000020ull)) {
        return 2;
    }
    if (is(i, 0xe290000000000000ull, 0xfff0000000000020ull) ||
        is(i, 0xe2a0000000000000ull, 0xfff0000000000020ull) ||
        is(i, 0xe2b0000000000000ull, 0xfff0000000000020ull)) {
        return 3;
    }
    if (is(i, 0xf0f8000000000000ull, 0xfff8000000000000ull) ||
        is(i, 0xe340000000000000ull, 0xfff0000000000000ull) ||
        is(i, 0xe350000000000000ull, 0xfff0000000000000ull)) {
        return 4;
    }
    if (is(i, 0x50b0000000000000ull, 0xfff8000000000000ull)) {
        return 5;
    }
    if (is(i, 0xefd8000000000000ull, 0xfff8000000000000ull) ||
        is(i, 0xeff0000000000000ull, 0xfff8000000000000ull)) {
        return 6;
    }
    if (is(i, 0xef90000000000000ull, 0xfff8000000000000ull)) {
        return 7;
    }
    if (is(i, 0xe000000000000000ull, 0xff00004000000000ull)) {
        return 8;
    }
    if (is(i, 0x5c90000000000000ull, 0xfff8000000000000ull) ||
        is(i, 0x4c90000000000000ull, 0xfff8000000000000ull) ||
        is(i, 0x3890000000000000ull, 0xfef8000000000000ull)) {
        return 9;
    }
    if (is(i, 0x5cb0000000000000ull, 0xfff8000000000000ull) ||
        is(i, 0x4cb0000000000000ull, 0xfff8000000000000ull) ||
        is(i, 0x38b0000000000000ull, 0xfef8000000000000ull)) {
        return 10;
    }
    if (is(i, 0x5cb8000000000000ull, 0xfff8000000000000ull) ||
        is(i, 0x4cb8000000000000ull, 0xfff8000000000000ull) ||
        is(i, 0x38b8000000000000ull, 0xfef8000000000000ull)) {
        return 11;
    }
    if (is(i, 0x5080000000000000ull, 0xfff8000000000000ull)) {
        return 12;
    }
    if (is(i, 0x0100000000000000ull, 0xfff0000000000000ull)) {
        return 13;
    }
    if (is(i, 0x5c98000000000000ull, 0xfff8000000000000ull) ||
        is(i, 0x4c98000000000000ull, 0xfff8000000000000ull) ||
        is(i, 0x3898000000000000ull, 0xfef8000000000000ull)) {
        return 14;
    }
    if (is(i, 0x5c58000000000000ull, 0xfff8000000000000ull) ||
        is(i, 0x4c58000000000000ull, 0xfff8000000000000ull) ||
        is(i, 0x3858000000000000ull, 0xfef8000000000000ull)) {
        return 15;
    }
    if (is(i, 0x5c68000000000000ull, 0xfff8000000000000ull) ||
        is(i, 0x4c68000000000000ull, 0xfff8000000000000ull) ||
        is(i, 0x3868000000000000ull, 0xfef8000000000000ull)) {
        return 16;
    }
    if (is(i, 0x5980000000000000ull, 0xff80000000000000ull) ||
        is(i, 0x4980000000000000ull, 0xff80000000000000ull) ||
        is(i, 0x5180000000000000ull, 0xff80000000000000ull) ||
        is(i, 0x3280000000000000ull, 0xfe80000000000000ull)) {
        return 17;
    }
    if (is(i, 0x0800000000000000ull, 0xfc00000000000000ull)) {
        return 18;
    }
    if (is(i, 0x1e00000000000000ull, 0xff00000000000000ull)) {
        return 19;
    }
    if (is(i, 0x0c00000000000000ull, 0xfc00000000000000ull)) {
        return 20;
    }
    if (is(i, 0x5c48000000000000ull, 0xfff8000000000000ull) ||
        is(i, 0x4c48000000000000ull, 0xfff8000000000000ull) ||
        is(i, 0x3848000000000000ull, 0xfef8000000000000ull)) {
        return 21;
    }
    if (is(i, 0x5c28000000000000ull, 0xfff8000000000000ull) ||
        is(i, 0x4c28000000000000ull, 0xfff8000000000000ull) ||
        is(i, 0x3828000000000000ull, 0xfef8000000000000ull)) {
        return 22;
    }
    if (is(i, 0x5c10000000000000ull, 0xfff8000000000000ull) ||
        is(i, 0x4c10000000000000ull, 0xfff8000000000000ull) ||
        is(i, 0x3810000000000000ull, 0xfef8000000000000ull)) {
        return 23;
    }
    if (is(i, 0x5c18000000000000ull, 0xfff8000000000000ull) ||
        is(i, 0x4c18000000000000ull, 0xfff8000000000000ull) ||
        is(i, 0x3818000000000000ull, 0xfef8000000000000ull)) {
        return 24;
    }
    if (is(i, 0x5c40000000000000ull, 0xfff8000000000000ull) ||
        is(i, 0x4c40000000000000ull, 0xfff8000000000000ull) ||
        is(i, 0x3840000000000000ull, 0xfef8000000000000ull) ||
        is(i, 0x0400000000000000ull, 0xfc00000000000000ull)) {
        return 25;
    }
    if (is(i, 0x5ca0000000000000ull, 0xfff8000000000000ull) ||
        is(i, 0x4ca0000000000000ull, 0xfff8000000000000ull) ||
        is(i, 0x38a0000000000000ull, 0xfef8000000000000ull)) {
        return 26;
    }
    if (is(i, 0x5c60000000000000ull, 0xfff8000000000000ull) ||
        is(i, 0x4c60000000000000ull, 0xfff8000000000000ull) ||
        is(i, 0x3860000000000000ull, 0xfef8000000000000ull)) {
        return 27;
    }
    if (is(i, 0x5bb0000000000000ull, 0xfff0000000000000ull) ||
        is(i, 0x4bb0000000000000ull, 0xfff0000000000000ull) ||
        is(i, 0x36b0000000000000ull, 0xfef0000000000000ull)) {
        return 28;
    }
    if (is(i, 0x5b60000000000000ull, 0xfff0000000000000ull) ||
        is(i, 0x4b60000000000000ull, 0xfff0000000000000ull) ||
        is(i, 0x3660000000000000ull, 0xfef0000000000000ull)) {
        return 29;
    }
    if (is(i, 0xd000000000000000ull, 0xf600000000000000ull)) {
        return 30;
    }
    if (is(i, 0xda00000000000000ull, 0xfe00000000000000ull)) {
        return 31;
    }
    if (is(i, 0xc038000000000000ull, 0xfc38000000000000ull)) {
        return 32;
    }
    return 0;
}

/* What every instruction needs before its handler: handler, guard, d and a. */
typedef struct decoded {
    uint8_t op;
    uint8_t guard; /* predicate [2:0], negated [3] */
    uint8_t d, ra;
} decoded;

struct shader_dcache {
    struct {
        u64 insn;
        decoded k; /* k.op is classify() + 1, 0 empty */
    } e[SHADER_DCACHE];
};

shader_dcache *shader_dcache_new(void) {
    return calloc(1, sizeof(shader_dcache));
}

/* Indexed by pc: a program's instructions never collide with each other, and the
 * word tag catches another program at the same pc (a hash of the word collided
 * often enough within one shader to miss on 14% of lookups). */
static INLINE decoded decode_fields(u64 i) {
    decoded k = {classify(i), (uint8_t)bits(i, 16, 4), (uint8_t)bits(i, 0, 8),
                 (uint8_t)bits(i, 8, 8)};
    return k;
}

static INLINE decoded decode(shader_dcache *c, uint32_t pc, u64 i) {
    if (!c) {
        return decode_fields(i);
    }
    uint32_t h = pc & (SHADER_DCACHE - 1);
    if (c->e[h].k.op && c->e[h].insn == i) {
        decoded k = c->e[h].k;
        k.op--;
        return k;
    }
    decoded k = decode_fields(i);
    c->e[h].insn = i;
    c->e[h].k = k;
    c->e[h].k.op++;
    return k;
}

int shader_run(const uint64_t *code, uint32_t code_words, shader_env *e, uint64_t *bad_insn) {
    uint32_t pc = 1;                                     /* word 0 is the first control word */
    uint32_t stack[3][CF_DEPTH], stack_n[3] = {0, 0, 0}; /* SSY/SYNC, PBK/BRK, PCNT/CONT */
    e->r[RZ] = 0;
    for (uint32_t step = 0; step < MAX_STEPS; step++) {
        if ((pc & 3) == 0) {
            pc++; /* control word */
        }
        if (pc >= code_words) {
            *bad_insn = 0;
            return -1;
        }
        u64 i = code[pc];
        decoded dk = decode(e->dcache, pc, i);
        uint32_t opc = dk.op;
        uint32_t next = pc + 1;
        int guard = pred(e, dk.guard & 7) ^ (dk.guard >> 3);
        uint32_t d = dk.d;
        uint32_t a = reg(e, dk.ra);

        if (!guard) {
            pc = next;
            continue;
        }

        if (opc == 1) { /* EXIT */
            return 0;
        } else if (opc == 2) { /* BRA rel */
            int32_t off = (int32_t)(bits(i, 20, 24) << 8) >> 8;
            int64_t target = (int64_t)pc * 8 + 8 + off;
            next = (uint32_t)(target / 8);
        } else if (opc == 3) { /* SSY, PBK, PCNT */
            /* One thread never diverges: these only push where SYNC/BRK/CONT will jump. */
            uint32_t kind = (uint32_t)((i >> 52) & 0xF) - 9;
            int32_t off = (int32_t)(bits(i, 20, 24) << 8) >> 8;
            if (stack_n[kind] < CF_DEPTH) {
                stack[kind][stack_n[kind]++] = (uint32_t)(((int64_t)pc * 8 + 8 + off) / 8);
            }
        } else if (opc == 4) { /* SYNC, BRK, CONT */
            uint32_t kind = (i >> 60) == 0xF ? 0 : (uint32_t)((i >> 52) & 0xF) - 3;
            if (stack_n[kind]) {
                /* CONT jumps back each iteration, so its target stays pushed. */
                next = kind == 2 ? stack[kind][stack_n[kind] - 1] : stack[kind][--stack_n[kind]];
            }
        } else if (opc == 5) { /* NOP */
        } else if (opc == 6) { /* ALD, AST */
            int store = is(i, 0xeff0000000000000ull, 0xfff8000000000000ull);
            uint32_t addr = (a + bits(i, 20, 10)) / 4, n = bits(i, 47, 2) + 1;
            for (uint32_t k = 0; k < n && addr + k < SHADER_ATTR_WORDS; k++) {
                if (store) {
                    e->out[addr + k] = reg(e, d == RZ ? RZ : d + k);
                } else {
                    set_reg(e, d == RZ ? RZ : d + k,
                            bits(i, 32, 1) ? e->out[addr + k] : e->in[addr + k]);
                }
            }
        } else if (opc == 7) { /* LDC */
            int32_t off = (int32_t)(bits(i, 20, 16) << 16) >> 16;
            uint32_t idx = bits(i, 36, 5), byte = a + (uint32_t)off, size = bits(i, 48, 3);
            uint32_t w = cbuf_word(e, idx, byte & ~3u);
            switch (size) {
            case 0:
                set_reg(e, d, (w >> (8 * (byte & 3))) & 0xFF);
                break;
            case 1:
                set_reg(e, d, (uint32_t)(int32_t)(int8_t)(w >> (8 * (byte & 3))));
                break;
            case 2:
                set_reg(e, d, (w >> (8 * (byte & 2))) & 0xFFFF);
                break;
            case 3:
                set_reg(e, d, (uint32_t)(int32_t)(int16_t)(w >> (8 * (byte & 2))));
                break;
            case 5:
                set_reg(e, d, w);
                set_reg(e, d == RZ ? RZ : d + 1, cbuf_word(e, idx, (byte & ~3u) + 4));
                break;
            default:
                set_reg(e, d, w);
                break;
            }
        } else if (opc == 8) { /* IPA */
            uint32_t mode = bits(i, 54, 2), addr = bits(i, 28, 10) + a;
            float v = e->ipa ? e->ipa(e->user, addr, mode == 1 ? 1 : mode) : 0.0f;
            if (mode == 1) {
                v *= f(reg(e, bits(i, 20, 8)));
            }
            set_reg(e, d, u(sat(v, bits(i, 51, 1))));
        } else if (opc == 9) { /* RRO */
            /* Range reduction ahead of MUFU sin/cos/ex2; our MUFU takes the plain value. */
            int form = (i >> 60) == 5 ? FORM_REG : (i >> 60) == 4 ? FORM_CBUF : FORM_IMM;
            float y = fmod_neg_abs(f(operand_b(e, i, form, 1)), bits(i, 45, 1), bits(i, 49, 1));
            set_reg(e, d, u(y));
        } else if (opc == 10) { /* F2I (32-bit) */
            int form = (i >> 60) == 5 ? FORM_REG : (i >> 60) == 4 ? FORM_CBUF : FORM_IMM;
            float y = fmod_neg_abs(f(operand_b(e, i, form, 1)), bits(i, 45, 1), bits(i, 49, 1));
            double v = y != y ? 0.0 : (double)y;
            switch (bits(i, 39, 2)) { /* rounding: nearest even, floor, ceil, trunc */
            case 0:
                v = nearbyint(v);
                break;
            case 1:
                v = floor(v);
                break;
            case 2:
                v = ceil(v);
                break;
            default:
                v = trunc(v);
                break;
            }
            uint32_t out;
            if (bits(i, 12, 1)) { /* signed result, saturating */
                out = (uint32_t)(int32_t)(v < -2147483648.0  ? -2147483648.0
                                          : v > 2147483647.0 ? 2147483647.0
                                                             : v);
            } else {
                out = (uint32_t)(v < 0.0 ? 0.0 : v > 4294967295.0 ? 4294967295.0 : v);
            }
            set_reg(e, d, out);
        } else if (opc == 11) { /* I2F (32-bit) */
            int form = (i >> 60) == 5 ? FORM_REG : (i >> 60) == 4 ? FORM_CBUF : FORM_IMM;
            uint32_t b = operand_b(e, i, form, 0);
            float y = bits(i, 13, 1) ? (float)(int32_t)b : (float)b;
            set_reg(e, d, u(fmod_neg_abs(y, bits(i, 45, 1), bits(i, 49, 1))));
        } else if (opc == 12) { /* MUFU */
            float x = fmod_neg_abs(f(a), bits(i, 48, 1), bits(i, 46, 1)), r;
            switch (bits(i, 20, 4)) {
            case 0:
                r = cosf(x);
                break;
            case 1:
                r = sinf(x);
                break;
            case 2:
                r = exp2f(x);
                break;
            case 3:
                r = log2f(x);
                break;
            case 4:
                r = 1.0f / x;
                break;
            case 5:
                r = 1.0f / sqrtf(x);
                break;
            case 8:
                r = sqrtf(x);
                break;
            default:
                *bad_insn = i;
                return -1;
            }
            set_reg(e, d, u(sat(r, bits(i, 50, 1))));
        } else if (opc == 13) { /* MOV32I */
            set_reg(e, d, bits(i, 20, 32));
        } else if (opc == 14) { /* MOV */
            int form = (i >> 60) == 5 ? FORM_REG : (i >> 60) == 4 ? FORM_CBUF : FORM_IMM;
            set_reg(e, d, operand_b(e, i, form, 0));
        } else if (opc == 15) { /* FADD */
            int form = (i >> 60) == 5 ? FORM_REG : (i >> 60) == 4 ? FORM_CBUF : FORM_IMM;
            float x = fmod_neg_abs(f(a), bits(i, 48, 1), bits(i, 46, 1));
            float y = fmod_neg_abs(f(operand_b(e, i, form, 1)), bits(i, 45, 1), bits(i, 49, 1));
            set_reg(e, d, u(sat(x + y, bits(i, 50, 1))));
        } else if (opc == 16) { /* FMUL */
            int form = (i >> 60) == 5 ? FORM_REG : (i >> 60) == 4 ? FORM_CBUF : FORM_IMM;
            float y = fmod_neg_abs(f(operand_b(e, i, form, 1)), bits(i, 48, 1), 0);
            set_reg(e, d, u(sat(f(a) * y, bits(i, 50, 1))));
        } else if (opc == 17) { /* FFMA */
            uint32_t bv, cv;
            if (is(i, 0x5980000000000000ull, 0xff80000000000000ull)) {
                bv = reg(e, bits(i, 20, 8));
                cv = reg(e, bits(i, 39, 8));
            } else if (is(i, 0x4980000000000000ull, 0xff80000000000000ull)) {
                bv = operand_b(e, i, FORM_CBUF, 1);
                cv = reg(e, bits(i, 39, 8));
            } else if (is(i, 0x5180000000000000ull, 0xff80000000000000ull)) {
                bv = reg(e, bits(i, 39, 8));
                cv = operand_b(e, i, FORM_CBUF, 1);
            } else {
                bv = imm20_float(i);
                cv = reg(e, bits(i, 39, 8));
            }
            float y = fmod_neg_abs(f(bv), bits(i, 48, 1), 0);
            float z = fmod_neg_abs(f(cv), bits(i, 49, 1), 0);
            set_reg(e, d, u(sat(fmaf(f(a), y, z), bits(i, 50, 1))));
        } else if (opc == 18) { /* FADD32I */
            float x = fmod_neg_abs(f(a), bits(i, 56, 1), bits(i, 54, 1));
            float y = fmod_neg_abs(f(bits(i, 20, 32)), bits(i, 53, 1), bits(i, 57, 1));
            set_reg(e, d, u(x + y));
        } else if (opc == 19) { /* FMUL32I */
            set_reg(e, d, u(sat(f(a) * f(bits(i, 20, 32)), bits(i, 55, 1))));
        } else if (opc == 20) { /* FFMA32I: d = a*imm + d */
            float x = fmod_neg_abs(f(a), bits(i, 56, 1), 0);
            float z = fmod_neg_abs(f(reg(e, d)), bits(i, 57, 1), 0);
            set_reg(e, d, u(sat(fmaf(x, f(bits(i, 20, 32)), z), bits(i, 55, 1))));
        } else if (opc == 21) { /* SHL */
            int form = (i >> 60) == 5 ? FORM_REG : (i >> 60) == 4 ? FORM_CBUF : FORM_IMM;
            uint32_t s = operand_b(e, i, form, 0);
            set_reg(e, d, bits(i, 39, 1) ? a << (s & 31) : (s > 31 ? 0 : a << s));
        } else if (opc == 22) { /* SHR */
            int form = (i >> 60) == 5 ? FORM_REG : (i >> 60) == 4 ? FORM_CBUF : FORM_IMM;
            uint32_t s = operand_b(e, i, form, 0);
            int is_signed = bits(i, 48, 1);
            if (bits(i, 39, 1)) {
                s &= 31;
            } else if (s > 31) {
                s = 31;
                if (!is_signed) {
                    set_reg(e, d, 0);
                    pc = next;
                    continue;
                }
            }
            set_reg(e, d, is_signed ? (uint32_t)((int32_t)a >> s) : a >> s);
        } else if (opc == 23) { /* IADD */
            int form = (i >> 60) == 5 ? FORM_REG : (i >> 60) == 4 ? FORM_CBUF : FORM_IMM;
            uint32_t x = bits(i, 49, 1) ? 0u - a : a;
            uint32_t b = operand_b(e, i, form, 0);
            set_reg(e, d, x + (bits(i, 48, 1) ? 0u - b : b));
        } else if (opc == 24) { /* ISCADD */
            int form = (i >> 60) == 5 ? FORM_REG : (i >> 60) == 4 ? FORM_CBUF : FORM_IMM;
            uint32_t x = (bits(i, 49, 1) ? 0u - a : a) << bits(i, 39, 5);
            uint32_t b = operand_b(e, i, form, 0);
            set_reg(e, d, x + (bits(i, 48, 1) ? 0u - b : b));
        } else if (opc == 25) { /* LOP, LOP32I */
            int i32 = is(i, 0x0400000000000000ull, 0xfc00000000000000ull);
            int form = (i >> 60) == 5 ? FORM_REG : (i >> 60) == 4 ? FORM_CBUF : FORM_IMM;
            uint32_t b = i32 ? bits(i, 20, 32) : operand_b(e, i, form, 0);
            uint32_t x = (i32 ? bits(i, 55, 1) : bits(i, 39, 1)) ? ~a : a;
            b = (i32 ? bits(i, 56, 1) : bits(i, 40, 1)) ? ~b : b;
            uint32_t op = i32 ? bits(i, 53, 2) : bits(i, 41, 2), r;
            r = op == 0 ? x & b : op == 1 ? x | b : op == 2 ? x ^ b : b;
            set_reg(e, d, r);
        } else if (opc == 26) { /* SEL */
            int form = (i >> 60) == 5 ? FORM_REG : (i >> 60) == 4 ? FORM_CBUF : FORM_IMM;
            int c = pred(e, bits(i, 39, 3)) ^ bits(i, 42, 1);
            set_reg(e, d, c ? a : operand_b(e, i, form, 0));
        } else if (opc == 27) { /* FMNMX */
            int form = (i >> 60) == 5 ? FORM_REG : (i >> 60) == 4 ? FORM_CBUF : FORM_IMM;
            float x = fmod_neg_abs(f(a), bits(i, 48, 1), bits(i, 46, 1));
            float y = fmod_neg_abs(f(operand_b(e, i, form, 1)), bits(i, 45, 1), bits(i, 49, 1));
            int min = pred(e, bits(i, 39, 3)) ^ bits(i, 42, 1);
            set_reg(e, d, u(min ? fminf(x, y) : fmaxf(x, y)));
        } else if (opc == 28) { /* FSETP */
            int form = (i >> 60) == 5 ? FORM_REG : (i >> 60) == 4 ? FORM_CBUF : FORM_IMM;
            float x = fmod_neg_abs(f(a), bits(i, 43, 1), bits(i, 7, 1));
            float y = fmod_neg_abs(f(operand_b(e, i, form, 1)), bits(i, 6, 1), bits(i, 44, 1));
            int r = fcmp(bits(i, 48, 4), x, y);
            int c = pred(e, bits(i, 39, 3)) ^ bits(i, 42, 1);
            uint32_t op = bits(i, 45, 2);
            set_pred(e, bits(i, 3, 3), combine(op, r, c));
            set_pred(e, bits(i, 0, 3), combine(op, !r, c));
        } else if (opc == 29) { /* ISETP */
            int form = (i >> 60) == 5 ? FORM_REG : (i >> 60) == 4 ? FORM_CBUF : FORM_IMM;
            int r = icmp(bits(i, 49, 3), a, operand_b(e, i, form, 0), bits(i, 48, 1));
            int c = pred(e, bits(i, 39, 3)) ^ bits(i, 42, 1);
            uint32_t op = bits(i, 45, 2);
            set_pred(e, bits(i, 3, 3), combine(op, r, c));
            set_pred(e, bits(i, 0, 3), combine(op, !r, c));
        } else if (opc == 30) { /* TEXS */
            /* dst1 [7:0] (+1), dst2 [35:28] (+1), u = A [15:8], v = B [27:20], texture index
             * [48:36], type/lod [56:53] (all 2D forms sample level 0 here), components [52:50]:
             * with dst2 = RZ one of r g b a rg ra ga ba, else rgb rga rba gba rgba. */
            static const uint8_t one[8][2] = {{0, 9}, {1, 9}, {2, 9}, {3, 9},
                                              {0, 1}, {0, 3}, {1, 3}, {2, 3}};
            static const uint8_t two[5][4] = {
                {0, 1, 2, 9}, {0, 1, 3, 9}, {0, 2, 3, 9}, {1, 2, 3, 9}, {0, 1, 2, 3}};
            uint32_t d2 = bits(i, 28, 8), sel = bits(i, 50, 3);
            int is_1d = bits(i, 53, 4) == 0;
            float texel[4] = {0, 0, 0, 0};
            if (e->tex) {
                e->tex(e->tex_user, bits(i, 36, 13), f(a), is_1d ? 0.0f : f(reg(e, bits(i, 20, 8))),
                       texel);
            }
            const uint8_t *comp = d2 == RZ ? one[sel] : (sel < 5 ? two[sel] : two[4]);
            uint32_t dst[4] = {d, d == RZ ? RZ : d + 1, d2, d2 == RZ ? RZ : d2 + 1};
            for (int k = 0; k < 4 && comp[k] != 9; k++) {
                set_reg(e, dst[k], u(texel[comp[k]]));
            }
        } else if (opc == 31) { /* TLDS */
            /* texelFetch, compact: registers and components as TEXS; [56:53] picks the form:
             * 0/1 1D (x = A), 2 2D (x = A, y = B), 4/5/6/12 2D (x = A, y = A+1, B holds an
             * offset, lod or sample we do not use). Level 0 only. */
            static const uint8_t one[8][2] = {{0, 9}, {1, 9}, {2, 9}, {3, 9},
                                              {0, 1}, {0, 3}, {1, 3}, {2, 3}};
            static const uint8_t two[5][4] = {
                {0, 1, 2, 9}, {0, 1, 3, 9}, {0, 2, 3, 9}, {1, 2, 3, 9}, {0, 1, 2, 3}};
            uint32_t d2 = bits(i, 28, 8), sel = bits(i, 50, 3), form = bits(i, 53, 4);
            uint32_t ra = bits(i, 8, 8);
            int32_t y = form <= 1   ? 0
                        : form == 2 ? (int32_t)reg(e, bits(i, 20, 8))
                                    : (int32_t)reg(e, ra == RZ ? RZ : ra + 1);
            float texel[4] = {0, 0, 0, 0};
            if (form != 2 && form != 4 && form != 5 && form != 6 && form != 12 && form > 1) {
                *bad_insn = i; /* 3D and array forms */
                return -1;
            }
            if (e->texel) {
                e->texel(e->tex_user, bits(i, 36, 13), (int32_t)a, y, texel);
            }
            const uint8_t *comp = d2 == RZ ? one[sel] : (sel < 5 ? two[sel] : two[4]);
            uint32_t dst[4] = {d, d == RZ ? RZ : d + 1, d2, d2 == RZ ? RZ : d2 + 1};
            for (int k = 0; k < 4 && comp[k] != 9; k++) {
                set_reg(e, dst[k], u(texel[comp[k]]));
            }
        } else if (opc == 32) { /* TEX */
            /* dst [7:0].., coordinates A [15:8] (u) and A+1 (v), index [48:36],
             * write mask [34:31] filling consecutive registers. */
            float texel[4] = {0, 0, 0, 0};
            uint32_t ra = bits(i, 8, 8);
            if (e->tex) {
                e->tex(e->tex_user, bits(i, 36, 13), f(a), f(reg(e, ra == RZ ? RZ : ra + 1)),
                       texel);
            }
            uint32_t mask = bits(i, 31, 4), k = 0;
            for (int c = 0; c < 4; c++) {
                if (mask & (1u << c)) {
                    set_reg(e, d == RZ ? RZ : d + k++, u(texel[c]));
                }
            }
        } else {
            *bad_insn = i;
            return -1;
        }
        pc = next;
    }
    *bad_insn = 0;
    return -1;
}

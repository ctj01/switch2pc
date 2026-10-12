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
#include <string.h>

#define RZ        255
#define PT        7
#define MAX_STEPS 1000000

typedef uint64_t u64;

static uint32_t bits(u64 i, unsigned lo, unsigned n) {
    return (uint32_t)((i >> lo) & ((n == 32) ? 0xFFFFFFFFull : ((1ull << n) - 1)));
}

static int is(u64 i, u64 value, u64 mask) {
    return (i & mask) == value;
}

static float f(uint32_t v) {
    float x;
    memcpy(&x, &v, 4);
    return x;
}

static uint32_t u(float x) {
    uint32_t v;
    memcpy(&v, &x, 4);
    return v;
}

static uint32_t reg(const shader_env *e, uint32_t n) {
    return n == RZ ? 0 : e->r[n];
}

static void set_reg(shader_env *e, uint32_t n, uint32_t v) {
    if (n != RZ) {
        e->r[n] = v;
    }
}

static int pred(const shader_env *e, uint32_t n) {
    return n == PT ? 1 : e->p[n & 7];
}

static void set_pred(shader_env *e, uint32_t n, int v) {
    if (n != PT) {
        e->p[n & 7] = (uint8_t)(v != 0);
    }
}

static uint32_t cbuf_word(const shader_env *e, uint32_t idx, uint32_t byte_off) {
    if (idx >= SHADER_CBUFS || !e->cb[idx] || byte_off + 4 > e->cb_size[idx]) {
        return 0;
    }
    uint32_t v;
    memcpy(&v, e->cb[idx] + byte_off, 4);
    return v;
}

/* Operand B of the reg (0x5c..), cbuf (0x4c..) and 20-bit immediate (0x38..) forms. */
enum { FORM_REG, FORM_CBUF, FORM_IMM };

static uint32_t imm20_int(u64 i) {
    uint32_t v = bits(i, 20, 19);
    return bits(i, 56, 1) ? v | 0xFFF80000u : v;
}

static uint32_t imm20_float(u64 i) {
    return (bits(i, 20, 19) | (bits(i, 56, 1) << 19)) << 12;
}

static uint32_t operand_b(const shader_env *e, u64 i, int form, int is_float) {
    switch (form) {
    case FORM_REG:
        return reg(e, bits(i, 20, 8));
    case FORM_CBUF:
        return cbuf_word(e, bits(i, 34, 5), bits(i, 20, 14) * 4);
    default:
        return is_float ? imm20_float(i) : imm20_int(i);
    }
}

static float fmod_neg_abs(float x, int neg, int abs_) {
    if (abs_) {
        x = fabsf(x);
    }
    return neg ? -x : x;
}

static float sat(float x, int on) {
    if (!on) {
        return x;
    }
    return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x != x ? 0.0f : x;
}

static int fcmp(uint32_t op, float a, float b) {
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

static int icmp(uint32_t op, uint32_t a, uint32_t b, int is_signed) {
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

static int combine(uint32_t op, int a, int b) {
    switch (op) {
    case 1:
        return a || b;
    case 2:
        return a ^ b;
    default:
        return a && b;
    }
}

int shader_run(const uint64_t *code, uint32_t code_words, shader_env *e, uint64_t *bad_insn) {
    uint32_t pc = 1; /* word 0 is the first control word */
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
        uint32_t next = pc + 1;
        int guard = pred(e, bits(i, 16, 3)) ^ bits(i, 19, 1);
        uint32_t d = bits(i, 0, 8);
        uint32_t a = reg(e, bits(i, 8, 8));

        if (!guard) {
            pc = next;
            continue;
        }

        if (is(i, 0xe300000000000000ull, 0xfff0000000000000ull)) { /* EXIT */
            return 0;
        } else if (is(i, 0xe240000000000000ull, 0xfff0000000000020ull)) { /* BRA rel */
            int32_t off = (int32_t)(bits(i, 20, 24) << 8) >> 8;
            int64_t target = (int64_t)pc * 8 + 8 + off;
            next = (uint32_t)(target / 8);
        } else if (is(i, 0x50b0000000000000ull, 0xfff8000000000000ull)) { /* NOP */
        } else if (is(i, 0xefd8000000000000ull, 0xfff8000000000000ull) || /* ALD */
                   is(i, 0xeff0000000000000ull, 0xfff8000000000000ull)) { /* AST */
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
        } else if (is(i, 0xef90000000000000ull, 0xfff8000000000000ull)) { /* LDC */
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
        } else if (is(i, 0xe000000000000000ull, 0xff00004000000000ull)) { /* IPA */
            uint32_t mode = bits(i, 54, 2), addr = bits(i, 28, 10) + a;
            float v = e->ipa ? e->ipa(e->user, addr, mode == 1 ? 1 : mode) : 0.0f;
            if (mode == 1) {
                v *= f(reg(e, bits(i, 20, 8)));
            }
            set_reg(e, d, u(sat(v, bits(i, 51, 1))));
        } else if (is(i, 0x5080000000000000ull, 0xfff8000000000000ull)) { /* MUFU */
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
        } else if (is(i, 0x0100000000000000ull, 0xfff0000000000000ull)) { /* MOV32I */
            set_reg(e, d, bits(i, 20, 32));
        } else if (is(i, 0x5c98000000000000ull, 0xfff8000000000000ull) ||
                   is(i, 0x4c98000000000000ull, 0xfff8000000000000ull) ||
                   is(i, 0x3898000000000000ull, 0xfef8000000000000ull)) { /* MOV */
            int form = (i >> 60) == 5 ? FORM_REG : (i >> 60) == 4 ? FORM_CBUF : FORM_IMM;
            set_reg(e, d, operand_b(e, i, form, 0));
        } else if (is(i, 0x5c58000000000000ull, 0xfff8000000000000ull) ||
                   is(i, 0x4c58000000000000ull, 0xfff8000000000000ull) ||
                   is(i, 0x3858000000000000ull, 0xfef8000000000000ull)) { /* FADD */
            int form = (i >> 60) == 5 ? FORM_REG : (i >> 60) == 4 ? FORM_CBUF : FORM_IMM;
            float x = fmod_neg_abs(f(a), bits(i, 48, 1), bits(i, 46, 1));
            float y = fmod_neg_abs(f(operand_b(e, i, form, 1)), bits(i, 45, 1), bits(i, 49, 1));
            set_reg(e, d, u(sat(x + y, bits(i, 50, 1))));
        } else if (is(i, 0x5c68000000000000ull, 0xfff8000000000000ull) ||
                   is(i, 0x4c68000000000000ull, 0xfff8000000000000ull) ||
                   is(i, 0x3868000000000000ull, 0xfef8000000000000ull)) { /* FMUL */
            int form = (i >> 60) == 5 ? FORM_REG : (i >> 60) == 4 ? FORM_CBUF : FORM_IMM;
            float y = fmod_neg_abs(f(operand_b(e, i, form, 1)), bits(i, 48, 1), 0);
            set_reg(e, d, u(sat(f(a) * y, bits(i, 50, 1))));
        } else if (is(i, 0x5980000000000000ull, 0xff80000000000000ull) ||
                   is(i, 0x4980000000000000ull, 0xff80000000000000ull) ||
                   is(i, 0x5180000000000000ull, 0xff80000000000000ull) ||
                   is(i, 0x3280000000000000ull, 0xfe80000000000000ull)) { /* FFMA */
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
        } else if (is(i, 0x0800000000000000ull, 0xfc00000000000000ull)) { /* FADD32I */
            float x = fmod_neg_abs(f(a), bits(i, 56, 1), bits(i, 54, 1));
            float y = fmod_neg_abs(f(bits(i, 20, 32)), bits(i, 53, 1), bits(i, 57, 1));
            set_reg(e, d, u(x + y));
        } else if (is(i, 0x1e00000000000000ull, 0xff00000000000000ull)) { /* FMUL32I */
            set_reg(e, d, u(sat(f(a) * f(bits(i, 20, 32)), bits(i, 55, 1))));
        } else if (is(i, 0x0c00000000000000ull,
                      0xfc00000000000000ull)) { /* FFMA32I: d = a*imm + d */
            float x = fmod_neg_abs(f(a), bits(i, 56, 1), 0);
            float z = fmod_neg_abs(f(reg(e, d)), bits(i, 57, 1), 0);
            set_reg(e, d, u(sat(fmaf(x, f(bits(i, 20, 32)), z), bits(i, 55, 1))));
        } else if (is(i, 0x5c48000000000000ull, 0xfff8000000000000ull) ||
                   is(i, 0x4c48000000000000ull, 0xfff8000000000000ull) ||
                   is(i, 0x3848000000000000ull, 0xfef8000000000000ull)) { /* SHL */
            int form = (i >> 60) == 5 ? FORM_REG : (i >> 60) == 4 ? FORM_CBUF : FORM_IMM;
            uint32_t s = operand_b(e, i, form, 0);
            set_reg(e, d, bits(i, 39, 1) ? a << (s & 31) : (s > 31 ? 0 : a << s));
        } else if (is(i, 0x5c28000000000000ull, 0xfff8000000000000ull) ||
                   is(i, 0x4c28000000000000ull, 0xfff8000000000000ull) ||
                   is(i, 0x3828000000000000ull, 0xfef8000000000000ull)) { /* SHR */
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
        } else if (is(i, 0x5c10000000000000ull, 0xfff8000000000000ull) ||
                   is(i, 0x4c10000000000000ull, 0xfff8000000000000ull) ||
                   is(i, 0x3810000000000000ull, 0xfef8000000000000ull)) { /* IADD */
            int form = (i >> 60) == 5 ? FORM_REG : (i >> 60) == 4 ? FORM_CBUF : FORM_IMM;
            uint32_t x = bits(i, 49, 1) ? 0u - a : a;
            uint32_t b = operand_b(e, i, form, 0);
            set_reg(e, d, x + (bits(i, 48, 1) ? 0u - b : b));
        } else if (is(i, 0x5c18000000000000ull, 0xfff8000000000000ull) ||
                   is(i, 0x4c18000000000000ull, 0xfff8000000000000ull) ||
                   is(i, 0x3818000000000000ull, 0xfef8000000000000ull)) { /* ISCADD */
            int form = (i >> 60) == 5 ? FORM_REG : (i >> 60) == 4 ? FORM_CBUF : FORM_IMM;
            uint32_t x = (bits(i, 49, 1) ? 0u - a : a) << bits(i, 39, 5);
            uint32_t b = operand_b(e, i, form, 0);
            set_reg(e, d, x + (bits(i, 48, 1) ? 0u - b : b));
        } else if (is(i, 0x5c40000000000000ull, 0xfff8000000000000ull) ||
                   is(i, 0x4c40000000000000ull, 0xfff8000000000000ull) ||
                   is(i, 0x3840000000000000ull, 0xfef8000000000000ull) ||
                   is(i, 0x0400000000000000ull, 0xfc00000000000000ull)) { /* LOP, LOP32I */
            int i32 = is(i, 0x0400000000000000ull, 0xfc00000000000000ull);
            int form = (i >> 60) == 5 ? FORM_REG : (i >> 60) == 4 ? FORM_CBUF : FORM_IMM;
            uint32_t b = i32 ? bits(i, 20, 32) : operand_b(e, i, form, 0);
            uint32_t x = (i32 ? bits(i, 55, 1) : bits(i, 39, 1)) ? ~a : a;
            b = (i32 ? bits(i, 56, 1) : bits(i, 40, 1)) ? ~b : b;
            uint32_t op = i32 ? bits(i, 53, 2) : bits(i, 41, 2), r;
            r = op == 0 ? x & b : op == 1 ? x | b : op == 2 ? x ^ b : b;
            set_reg(e, d, r);
        } else if (is(i, 0x5ca0000000000000ull, 0xfff8000000000000ull) ||
                   is(i, 0x4ca0000000000000ull, 0xfff8000000000000ull) ||
                   is(i, 0x38a0000000000000ull, 0xfef8000000000000ull)) { /* SEL */
            int form = (i >> 60) == 5 ? FORM_REG : (i >> 60) == 4 ? FORM_CBUF : FORM_IMM;
            int c = pred(e, bits(i, 39, 3)) ^ bits(i, 42, 1);
            set_reg(e, d, c ? a : operand_b(e, i, form, 0));
        } else if (is(i, 0x5c60000000000000ull, 0xfff8000000000000ull) ||
                   is(i, 0x4c60000000000000ull, 0xfff8000000000000ull) ||
                   is(i, 0x3860000000000000ull, 0xfef8000000000000ull)) { /* FMNMX */
            int form = (i >> 60) == 5 ? FORM_REG : (i >> 60) == 4 ? FORM_CBUF : FORM_IMM;
            float x = fmod_neg_abs(f(a), bits(i, 48, 1), bits(i, 46, 1));
            float y = fmod_neg_abs(f(operand_b(e, i, form, 1)), bits(i, 45, 1), bits(i, 49, 1));
            int min = pred(e, bits(i, 39, 3)) ^ bits(i, 42, 1);
            set_reg(e, d, u(min ? fminf(x, y) : fmaxf(x, y)));
        } else if (is(i, 0x5bb0000000000000ull, 0xfff0000000000000ull) ||
                   is(i, 0x4bb0000000000000ull, 0xfff0000000000000ull) ||
                   is(i, 0x36b0000000000000ull, 0xfef0000000000000ull)) { /* FSETP */
            int form = (i >> 60) == 5 ? FORM_REG : (i >> 60) == 4 ? FORM_CBUF : FORM_IMM;
            float x = fmod_neg_abs(f(a), bits(i, 43, 1), bits(i, 7, 1));
            float y = fmod_neg_abs(f(operand_b(e, i, form, 1)), bits(i, 6, 1), bits(i, 44, 1));
            int r = fcmp(bits(i, 48, 4), x, y);
            int c = pred(e, bits(i, 39, 3)) ^ bits(i, 42, 1);
            uint32_t op = bits(i, 45, 2);
            set_pred(e, bits(i, 3, 3), combine(op, r, c));
            set_pred(e, bits(i, 0, 3), combine(op, !r, c));
        } else if (is(i, 0x5b60000000000000ull, 0xfff0000000000000ull) ||
                   is(i, 0x4b60000000000000ull, 0xfff0000000000000ull) ||
                   is(i, 0x3660000000000000ull, 0xfef0000000000000ull)) { /* ISETP */
            int form = (i >> 60) == 5 ? FORM_REG : (i >> 60) == 4 ? FORM_CBUF : FORM_IMM;
            int r = icmp(bits(i, 49, 3), a, operand_b(e, i, form, 0), bits(i, 48, 1));
            int c = pred(e, bits(i, 39, 3)) ^ bits(i, 42, 1);
            uint32_t op = bits(i, 45, 2);
            set_pred(e, bits(i, 3, 3), combine(op, r, c));
            set_pred(e, bits(i, 0, 3), combine(op, !r, c));
        } else {
            *bad_insn = i;
            return -1;
        }
        pc = next;
    }
    *bad_insn = 0;
    return -1;
}

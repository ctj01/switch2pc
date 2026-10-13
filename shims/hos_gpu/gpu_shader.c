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
#include <pthread.h>
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

/* RZ and PT are slots that hold 0 and 1 for the whole run; writes to them go to
 * spare slots, so reads are plain loads. */
static INLINE uint32_t reg(const shader_env *e, uint32_t n) {
    return e->r[n];
}

static INLINE void set_reg(shader_env *e, uint32_t n, uint32_t v) {
    e->r[n >= RZ ? 256 : n] = v;
}

static INLINE int pred(const shader_env *e, uint32_t n) {
    return e->p[n & 7];
}

static INLINE void set_pred(shader_env *e, uint32_t n, int v) {
    e->p[(n & 7) == PT ? 8 : (n & 7)] = (uint8_t)(v != 0);
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

/* Decoding: classify() maps an instruction word to its handler (OP_*, 0 unknown). */
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
    if (is(i, 0xf0c8000000000000ull, 0xfff8000000000000ull)) {
        return 33;
    }
    if (is(i, 0xeed8000000000000ull, 0xfff8000000000000ull) ||
        is(i, 0xeed0000000000000ull, 0xfff8000000000000ull)) {
        return 34;
    }
    return 0;
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
    OP_CTRL, /* a scheduling control word */
};
_Static_assert(OP_GMEM == 34, "OP_* must follow classify()'s numbering");

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
    uint32_t refs; /* under g_progs_lock */
    struct shader_prog *next;
};

static INLINE uint32_t opnd_b(const shader_env *e, const pinsn *in) {
    if (in->bk == BK_REG) {
        return e->r[in->rb];
    }
    if (in->bk == BK_CBUF) {
        return cbuf_word(e, in->cbi, in->b);
    }
    return in->b;
}

static void predecode(u64 i, pinsn *in) {
    memset(in, 0, sizeof(*in));
    in->raw = i;
    in->op = classify(i);
    in->guard = (uint8_t)bits(i, 16, 4);
    in->d = (uint8_t)bits(i, 0, 8);
    in->ra = (uint8_t)bits(i, 8, 8);
    in->rc = (uint8_t)bits(i, 39, 8);
    in->cbi = (uint8_t)bits(i, 34, 5);
    int has_b = 0, is_float = 0;
    switch (in->op) {
    case OP_RRO:
    case OP_F2I:
    case OP_FADD:
    case OP_FMUL:
    case OP_FMNMX:
    case OP_FSETP:
        has_b = is_float = 1;
        break;
    case OP_I2F:
    case OP_MOV:
    case OP_SHL:
    case OP_SHR:
    case OP_IADD:
    case OP_ISCADD:
    case OP_SEL:
    case OP_ISETP:
        has_b = 1;
        break;
    case OP_LOP:
        if (is(i, 0x0400000000000000ull, 0xfc00000000000000ull)) { /* LOP32I */
            in->bk = BK_IMM;
            in->b = bits(i, 20, 32);
        } else {
            has_b = 1;
        }
        break;
    case OP_FFMA:
        if (is(i, 0x5980000000000000ull, 0xff80000000000000ull)) { /* B reg, C reg */
            in->bk = BK_REG;
            in->rb = (uint8_t)bits(i, 20, 8);
        } else if (is(i, 0x4980000000000000ull, 0xff80000000000000ull)) { /* B cbuf */
            in->bk = BK_CBUF;
            in->b = bits(i, 20, 14) * 4;
        } else if (is(i, 0x5180000000000000ull, 0xff80000000000000ull)) { /* B reg C, C cbuf */
            in->bk = BK_REG;
            in->rb = in->rc;
            in->ck = 1;
            in->b = bits(i, 20, 14) * 4;
        } else {
            in->bk = BK_IMM;
            in->b = imm20_float(i);
        }
        break;
    default:
        break;
    }
    if (has_b) {
        if ((i >> 60) == 5) {
            in->bk = BK_REG;
            in->rb = (uint8_t)bits(i, 20, 8);
        } else if ((i >> 60) == 4) {
            in->bk = BK_CBUF;
            in->b = bits(i, 20, 14) * 4;
        } else {
            in->bk = BK_IMM;
            in->b = is_float ? imm20_float(i) : imm20_int(i);
        }
    }
}

/* Words up to the last instruction execution can reach: an unconditional EXIT or
 * backward BRA with no branch target beyond it. Everything after is padding. */
static uint32_t prog_length(const uint64_t *code, uint32_t words) {
    uint32_t maxt = 0;
    for (uint32_t pc = 1; pc < words; pc++) {
        if ((pc & 3) == 0) {
            continue;
        }
        u64 i = code[pc];
        uint8_t op = classify(i);
        int always = bits(i, 16, 4) == PT;
        if (op == OP_BRA || op == OP_SSY) {
            int64_t t = ((int64_t)pc * 8 + 8 + ((int32_t)(bits(i, 20, 24) << 8) >> 8)) / 8;
            if (t > (int64_t)maxt) {
                maxt = t < (int64_t)words ? (uint32_t)t : words;
            }
        }
        if (always && (op == OP_EXIT || op == OP_BRA) && pc >= maxt) {
            return pc + 1;
        }
    }
    return words;
}

static shader_prog *prog_build(const uint64_t *code, uint32_t n) {
    shader_prog *p = calloc(1, sizeof(*p));
    if (!p) {
        return NULL;
    }
    p->words = malloc((size_t)n * sizeof(*p->words));
    p->ins = malloc((size_t)n * sizeof(*p->ins));
    if (!p->words || !p->ins) {
        free(p->words);
        free(p->ins);
        free(p);
        return NULL;
    }
    memcpy(p->words, code, (size_t)n * sizeof(*p->words));
    for (uint32_t pc = 0; pc < n; pc++) {
        if ((pc & 3) == 0) {
            memset(&p->ins[pc], 0, sizeof(p->ins[pc]));
            p->ins[pc].op = OP_CTRL;
            p->ins[pc].guard = PT;
        } else {
            predecode(code[pc], &p->ins[pc]);
        }
    }
    p->code = code;
    p->n = n;
    return p;
}

static void prog_free(shader_prog *p) {
    free(p->words);
    free(p->ins);
    free(p);
}

#define PROG_CACHE 256 /* programs kept while unused */

static pthread_mutex_t g_progs_lock = PTHREAD_MUTEX_INITIALIZER;
static shader_prog *g_progs;
static uint32_t g_nprogs;

shader_prog *shader_prog_get(const uint64_t *code, uint32_t code_words) {
    uint32_t n = prog_length(code, code_words);
    pthread_mutex_lock(&g_progs_lock);
    for (shader_prog *p = g_progs; p; p = p->next) {
        if (p->code == code && p->n == n && memcmp(p->words, code, (size_t)n * 8) == 0) {
            p->refs++;
            pthread_mutex_unlock(&g_progs_lock);
            return p;
        }
    }
    if (g_nprogs >= PROG_CACHE) { /* drop every program nobody is running */
        for (shader_prog **pp = &g_progs; *pp;) {
            if ((*pp)->refs == 0) {
                shader_prog *dead = *pp;
                *pp = dead->next;
                prog_free(dead);
                g_nprogs--;
            } else {
                pp = &(*pp)->next;
            }
        }
    }
    shader_prog *p = prog_build(code, n);
    if (p) {
        p->refs = 1;
        p->next = g_progs;
        g_progs = p;
        g_nprogs++;
    }
    pthread_mutex_unlock(&g_progs_lock);
    return p;
}

void shader_prog_release(shader_prog *p) {
    if (!p) {
        return;
    }
    pthread_mutex_lock(&g_progs_lock);
    p->refs--;
    pthread_mutex_unlock(&g_progs_lock);
}

int shader_run(const uint64_t *code, uint32_t code_words, shader_env *e, uint64_t *bad_insn) {
    shader_prog *p = shader_prog_get(code, code_words);
    if (!p) {
        *bad_insn = 0;
        return -1;
    }
    int rc = shader_exec(p, e, bad_insn);
    shader_prog_release(p);
    return rc;
}

int shader_exec(const shader_prog *prog, shader_env *e, uint64_t *bad_insn) {
    const pinsn *ins = prog->ins;
    uint32_t nwords = prog->n;
    uint32_t pc = 1;                                     /* word 0 is the first control word */
    uint32_t stack[3][CF_DEPTH], stack_n[3] = {0, 0, 0}; /* SSY/SYNC, PBK/BRK, PCNT/CONT */
    e->r[RZ] = 0;
    e->p[PT] = 1;
    for (uint32_t step = 0; step < MAX_STEPS; step++) {
        if (pc >= nwords) {
            *bad_insn = 0;
            return -1;
        }
        const pinsn *in = &ins[pc];
        uint32_t next = pc + 1;
        if (in->guard != PT && !(e->p[in->guard & 7] ^ (in->guard >> 3))) {
            pc = next;
            continue;
        }
        u64 i = in->raw;
        uint32_t d = in->d;
        uint32_t a = e->r[in->ra];
        switch (in->op) {
        case OP_EXIT: { /* EXIT */
            return 0;
            break;
        }
        case OP_BRA: { /* BRA rel */
            int32_t off = (int32_t)(bits(i, 20, 24) << 8) >> 8;
            int64_t target = (int64_t)pc * 8 + 8 + off;
            next = (uint32_t)(target / 8);
            break;
        }
        case OP_SSY: { /* SSY, PBK, PCNT */
            /* One thread never diverges: these only push where SYNC/BRK/CONT will jump. */
            uint32_t kind = (uint32_t)((i >> 52) & 0xF) - 9;
            int32_t off = (int32_t)(bits(i, 20, 24) << 8) >> 8;
            if (stack_n[kind] < CF_DEPTH) {
                stack[kind][stack_n[kind]++] = (uint32_t)(((int64_t)pc * 8 + 8 + off) / 8);
            }
            break;
        }
        case OP_SYNC: { /* SYNC, BRK, CONT */
            uint32_t kind = (i >> 60) == 0xF ? 0 : (uint32_t)((i >> 52) & 0xF) - 3;
            if (stack_n[kind]) {
                /* CONT jumps back each iteration, so its target stays pushed. */
                next = kind == 2 ? stack[kind][stack_n[kind] - 1] : stack[kind][--stack_n[kind]];
            }
            break;
        }
        case OP_NOP: { /* NOP */
            break;
        }
        case OP_ATTR: { /* ALD, AST */
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
            break;
        }
        case OP_LDC: { /* LDC */
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
            break;
        }
        case OP_IPA: { /* IPA */
            uint32_t mode = bits(i, 54, 2), addr = bits(i, 28, 10) + a;
            float v = e->ipa ? e->ipa(e->user, addr, mode == 1 ? 1 : mode) : 0.0f;
            if (mode == 1) {
                v *= f(reg(e, bits(i, 20, 8)));
            }
            set_reg(e, d, u(sat(v, bits(i, 51, 1))));
            break;
        }
        case OP_RRO: { /* RRO */
            /* Range reduction ahead of MUFU sin/cos/ex2; our MUFU takes the plain value. */
            float y = fmod_neg_abs(f(opnd_b(e, in)), bits(i, 45, 1), bits(i, 49, 1));
            set_reg(e, d, u(y));
            break;
        }
        case OP_F2I: { /* F2I (32-bit) */
            float y = fmod_neg_abs(f(opnd_b(e, in)), bits(i, 45, 1), bits(i, 49, 1));
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
            break;
        }
        case OP_I2F: { /* I2F (32-bit) */
            uint32_t b = opnd_b(e, in);
            float y = bits(i, 13, 1) ? (float)(int32_t)b : (float)b;
            set_reg(e, d, u(fmod_neg_abs(y, bits(i, 45, 1), bits(i, 49, 1))));
            break;
        }
        case OP_MUFU: { /* MUFU */
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
            break;
        }
        case OP_MOV32I: { /* MOV32I */
            set_reg(e, d, bits(i, 20, 32));
            break;
        }
        case OP_MOV: { /* MOV */
            set_reg(e, d, opnd_b(e, in));
            break;
        }
        case OP_FADD: { /* FADD */
            float x = fmod_neg_abs(f(a), bits(i, 48, 1), bits(i, 46, 1));
            float y = fmod_neg_abs(f(opnd_b(e, in)), bits(i, 45, 1), bits(i, 49, 1));
            set_reg(e, d, u(sat(x + y, bits(i, 50, 1))));
            break;
        }
        case OP_FMUL: { /* FMUL */
            float y = fmod_neg_abs(f(opnd_b(e, in)), bits(i, 48, 1), 0);
            set_reg(e, d, u(sat(f(a) * y, bits(i, 50, 1))));
            break;
        }
        case OP_FFMA: { /* FFMA */
            uint32_t bv = opnd_b(e, in);
            uint32_t cv = in->ck ? cbuf_word(e, in->cbi, in->b) : reg(e, in->rc);
            float y = fmod_neg_abs(f(bv), bits(i, 48, 1), 0);
            float z = fmod_neg_abs(f(cv), bits(i, 49, 1), 0);
            set_reg(e, d, u(sat(fmaf(f(a), y, z), bits(i, 50, 1))));
            break;
        }
        case OP_FADD32I: { /* FADD32I */
            float x = fmod_neg_abs(f(a), bits(i, 56, 1), bits(i, 54, 1));
            float y = fmod_neg_abs(f(bits(i, 20, 32)), bits(i, 53, 1), bits(i, 57, 1));
            set_reg(e, d, u(x + y));
            break;
        }
        case OP_FMUL32I: { /* FMUL32I */
            set_reg(e, d, u(sat(f(a) * f(bits(i, 20, 32)), bits(i, 55, 1))));
            break;
        }
        case OP_FFMA32I: { /* FFMA32I: d = a*imm + d */
            float x = fmod_neg_abs(f(a), bits(i, 56, 1), 0);
            float z = fmod_neg_abs(f(reg(e, d)), bits(i, 57, 1), 0);
            set_reg(e, d, u(sat(fmaf(x, f(bits(i, 20, 32)), z), bits(i, 55, 1))));
            break;
        }
        case OP_SHL: { /* SHL */
            uint32_t s = opnd_b(e, in);
            set_reg(e, d, bits(i, 39, 1) ? a << (s & 31) : (s > 31 ? 0 : a << s));
            break;
        }
        case OP_SHR: { /* SHR */
            uint32_t s = opnd_b(e, in);
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
            break;
        }
        case OP_IADD: { /* IADD */
            uint32_t x = bits(i, 49, 1) ? 0u - a : a;
            uint32_t b = opnd_b(e, in);
            b = bits(i, 48, 1) ? 0u - b : b;
            /* .X [43] adds the carry, .CC [47] sets it: 64-bit address arithmetic */
            uint64_t sum = (uint64_t)x + b + (bits(i, 43, 1) ? e->carry : 0);
            if (bits(i, 47, 1)) {
                e->carry = (uint32_t)(sum >> 32) & 1;
            }
            set_reg(e, d, (uint32_t)sum);
            break;
        }
        case OP_ISCADD: { /* ISCADD */
            uint32_t x = (bits(i, 49, 1) ? 0u - a : a) << bits(i, 39, 5);
            uint32_t b = opnd_b(e, in);
            set_reg(e, d, x + (bits(i, 48, 1) ? 0u - b : b));
            break;
        }
        case OP_LOP: { /* LOP, LOP32I */
            int i32 = is(i, 0x0400000000000000ull, 0xfc00000000000000ull);
            uint32_t b = opnd_b(e, in); /* LOP32I: the 32-bit immediate */
            uint32_t x = (i32 ? bits(i, 55, 1) : bits(i, 39, 1)) ? ~a : a;
            b = (i32 ? bits(i, 56, 1) : bits(i, 40, 1)) ? ~b : b;
            uint32_t op = i32 ? bits(i, 53, 2) : bits(i, 41, 2), r;
            r = op == 0 ? x & b : op == 1 ? x | b : op == 2 ? x ^ b : b;
            set_reg(e, d, r);
            break;
        }
        case OP_SEL: { /* SEL */
            int c = pred(e, bits(i, 39, 3)) ^ bits(i, 42, 1);
            set_reg(e, d, c ? a : opnd_b(e, in));
            break;
        }
        case OP_FMNMX: { /* FMNMX */
            float x = fmod_neg_abs(f(a), bits(i, 48, 1), bits(i, 46, 1));
            float y = fmod_neg_abs(f(opnd_b(e, in)), bits(i, 45, 1), bits(i, 49, 1));
            int min = pred(e, bits(i, 39, 3)) ^ bits(i, 42, 1);
            set_reg(e, d, u(min ? fminf(x, y) : fmaxf(x, y)));
            break;
        }
        case OP_FSETP: { /* FSETP */
            float x = fmod_neg_abs(f(a), bits(i, 43, 1), bits(i, 7, 1));
            float y = fmod_neg_abs(f(opnd_b(e, in)), bits(i, 6, 1), bits(i, 44, 1));
            int r = fcmp(bits(i, 48, 4), x, y);
            int c = pred(e, bits(i, 39, 3)) ^ bits(i, 42, 1);
            uint32_t op = bits(i, 45, 2);
            set_pred(e, bits(i, 3, 3), combine(op, r, c));
            set_pred(e, bits(i, 0, 3), combine(op, !r, c));
            break;
        }
        case OP_ISETP: { /* ISETP */
            int r = icmp(bits(i, 49, 3), a, opnd_b(e, in), bits(i, 48, 1));
            int c = pred(e, bits(i, 39, 3)) ^ bits(i, 42, 1);
            uint32_t op = bits(i, 45, 2);
            set_pred(e, bits(i, 3, 3), combine(op, r, c));
            set_pred(e, bits(i, 0, 3), combine(op, !r, c));
            break;
        }
        case OP_TEXS: { /* TEXS */
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
            break;
        }
        case OP_TLDS: { /* TLDS */
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
            break;
        }
        case OP_TEX: { /* TEX */
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
            break;
        }
        case OP_S2R: { /* S2R */
            uint32_t sr = bits(i, 20, 8), v = 0;
            if (sr >= 0x21 && sr <= 0x23) { /* SR_TID.X/Y/Z */
                v = e->tid[sr - 0x21];
            } else if (sr >= 0x25 && sr <= 0x27) { /* SR_CTAID.X/Y/Z */
                v = e->ctaid[sr - 0x25];
            } else if (sr == 0x20) { /* SR_TID packed: x [15:0], y [25:16], z [31:26] */
                v = e->tid[0] | (e->tid[1] << 16) | (e->tid[2] << 26);
            } else if (sr != 0x00) { /* SR_LANEID reads 0 */
                *bad_insn = i;
                return -1;
            }
            set_reg(e, d, v);
            break;
        }
        case OP_GMEM: { /* LDG, STG */
            /* data [7:0], address A [15:8] (A, A+1 with .E [45]), offset [43:20] signed,
             * size [50:48]: u8 s8 u16 s16 b32 b64 b128. */
            static const uint8_t bytes[8] = {1, 1, 2, 2, 4, 8, 16, 16};
            uint32_t ra = bits(i, 8, 8), size = bits(i, 48, 3), n = bytes[size];
            uint64_t va = reg(e, ra);
            if (bits(i, 45, 1) && ra != RZ) {
                va |= (uint64_t)reg(e, ra + 1) << 32;
            }
            va += (uint64_t)(int64_t)((int32_t)(bits(i, 20, 24) << 8) >> 8);
            uint8_t *p = e->mem ? e->mem(va, n) : NULL;
            int store = is(i, 0xeed8000000000000ull, 0xfff8000000000000ull);
            if (!p) {
                *bad_insn = i; /* unmapped: stop rather than drop the access */
                return -1;
            }
            if (store) {
                for (uint32_t k = 0; k < (n + 3) / 4; k++) {
                    uint32_t v = reg(e, d == RZ ? RZ : d + k);
                    memcpy(p + 4 * k, &v, n < 4 ? n : 4);
                }
            } else if (n < 4) {
                uint32_t v = n == 1 ? p[0] : (uint32_t)(p[0] | p[1] << 8);
                if (size == 1) {
                    v = (uint32_t)(int32_t)(int8_t)v;
                } else if (size == 3) {
                    v = (uint32_t)(int32_t)(int16_t)v;
                }
                set_reg(e, d, v);
            } else {
                for (uint32_t k = 0; k < n / 4; k++) {
                    uint32_t v;
                    memcpy(&v, p + 4 * k, 4);
                    set_reg(e, d == RZ ? RZ : d + k, v);
                }
            }
            break;
        }
        case OP_CTRL:
            break;
        default:
            *bad_insn = i;
            return -1;
        }
        pc = next;
    }
    *bad_insn = 0;
    return -1;
}

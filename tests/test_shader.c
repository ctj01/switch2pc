/*
 * The Maxwell shader interpreter against hand-assembled code. Encodings are
 * those of envytools' gm107 tables (see gpu_shader.c); every instruction
 * carries the PT guard (bits 18:16 = 7).
 */
#include <string.h>

#include "gpu_shader.h"
#include "test.h"

#define PT_GUARD  (7ull << 16)
#define D(r)      ((uint64_t)(r))
#define A(r)      ((uint64_t)(r) << 8)
#define B(r)      ((uint64_t)(r) << 20)
#define C(r)      ((uint64_t)(r) << 39)
#define RZ        255
#define OPC(hi16) ((uint64_t)(hi16) << 48)

static uint64_t imm20f(float x) {
    uint32_t v;
    memcpy(&v, &x, 4);
    uint32_t t = v >> 12; /* top 20 bits: [18:0] at bit 20, [19] at bit 56 */
    return ((uint64_t)(t & 0x7FFFF) << 20) | ((uint64_t)(t >> 19) << 56);
}

static uint64_t imm20i(int32_t x) {
    return ((uint64_t)((uint32_t)x & 0x7FFFF) << 20) | ((uint64_t)(((uint32_t)x >> 19) & 1) << 56);
}

/* Lay instructions out with a control word in front of every three. */
static uint32_t assemble(const uint64_t *insns, uint32_t n, uint64_t *code) {
    uint32_t w = 0;
    for (uint32_t i = 0; i < n; i++) {
        if ((w & 3) == 0) {
            code[w++] = 0; /* control word */
        }
        code[w++] = insns[i];
    }
    return w;
}

static float fl(uint32_t v) {
    float x;
    memcpy(&x, &v, 4);
    return x;
}

TEST(shader_float_arithmetic) {
    const uint64_t insns[] = {
        OPC(0x0100) | PT_GUARD | D(1) | ((uint64_t)0x3f800000 << 20), /* mov32i r1, 1.0 */
        OPC(0x3858) | PT_GUARD | D(2) | A(1) | imm20f(2.0f),          /* fadd r2 = r1 + 2 */
        OPC(0x5c68) | PT_GUARD | D(3) | A(2) | B(2),                  /* fmul r3 = r2 * r2 */
        OPC(0x3280) | PT_GUARD | D(4) | A(3) | imm20f(0.5f) | C(1),   /* ffma r4 = r3*.5+r1 */
        OPC(0x5c58) | PT_GUARD | D(5) | A(4) | B(1) | (1ull << 45),   /* fadd r5 = r4 - r1 */
        OPC(0x5080) | PT_GUARD | D(6) | A(2) | (4ull << 20),          /* mufu rcp r6 = 1/r2 */
        OPC(0xeff0) | PT_GUARD | D(3) | A(RZ) | (0x80ull << 20) | (3ull << 47) |
            C(RZ),              /* st b128 */
        OPC(0xe300) | PT_GUARD, /* exit */
    };
    uint64_t code[16];
    uint32_t n = assemble(insns, sizeof(insns) / sizeof(insns[0]), code);
    static shader_env e;
    memset(&e, 0, sizeof(e));
    uint64_t bad = 0;
    ASSERT_EQ(shader_run(code, n, &e, &bad), 0);
    ASSERT_TRUE(fl(e.out[0x80 / 4 + 0]) == 9.0f); /* r3 */
    ASSERT_TRUE(fl(e.out[0x80 / 4 + 1]) == 5.5f); /* r4 */
    ASSERT_TRUE(fl(e.out[0x80 / 4 + 2]) == 4.5f); /* r5 */
    ASSERT_TRUE(fl(e.out[0x80 / 4 + 3]) > 0.333f && fl(e.out[0x80 / 4 + 3]) < 0.334f);
}

TEST(shader_integer_loop_with_predicated_branch) {
    /* r5 = 0; do { r5 += 1 } while (r5 < 10); out[0x80] = r5 */
    const uint64_t insns[] = {
        OPC(0x0100) | PT_GUARD | D(5),                                         /* 0: mov32i r5, 0 */
        OPC(0x3810) | PT_GUARD | D(5) | A(5) | imm20i(1),                      /* 1: iadd r5 += 1 */
        OPC(0x3660) | PT_GUARD | (1ull << 49) | (1ull << 48) | (0ull << 3) | 7 /* p0 = r5 < 10 */
            | A(5) | imm20i(10) | (7ull << 39),
        0,                                                               /* 3: bra filled below */
        OPC(0xeff0) | PT_GUARD | D(5) | A(RZ) | (0x80ull << 20) | C(RZ), /* 4: st a[0x80] */
        OPC(0xe300) | PT_GUARD,                                          /* 5: exit */
    };
    uint64_t ins[6];
    memcpy(ins, insns, sizeof(ins));
    uint64_t code[16];
    /* Layout: words 0 ctrl, 1 i0, 2 i1, 3 i2, 4 ctrl, 5 i3 (bra), ...; target i1 at word 2. */
    int32_t off = (int32_t)(2 * 8) - (int32_t)(5 * 8 + 8);
    ins[3] = 0xe240000000000000ull | (0ull << 16) /* @p0 */ |
             ((uint64_t)((uint32_t)off & 0xFFFFFF) << 20);
    uint32_t n = assemble(ins, 6, code);
    static shader_env e;
    memset(&e, 0, sizeof(e));
    uint64_t bad = 0;
    ASSERT_EQ(shader_run(code, n, &e, &bad), 0);
    ASSERT_EQ(e.out[0x80 / 4], 10);
}

TEST(shader_attributes_and_constant_buffer) {
    static const uint32_t cb1[4] = {11, 22, 33, 44};
    const uint64_t insns[] = {
        OPC(0xefd8) | PT_GUARD | D(0) | A(RZ) | (0x2fcull << 20) | C(RZ), /* ld r0 = a[0x2fc] */
        OPC(0x3848) | PT_GUARD | D(7) | A(0) | imm20i(2),                 /* shl r7 = r0 << 2 */
        OPC(0xef90) | PT_GUARD | D(1) | A(7) | (4ull << 20) | (1ull << 36) |
            (4ull << 48),                                                /* c1[r7+4] */
        OPC(0xeff0) | PT_GUARD | D(1) | A(RZ) | (0x90ull << 20) | C(RZ), /* st a[0x90] = r1 */
        OPC(0xe300) | PT_GUARD,
    };
    uint64_t code[16];
    uint32_t n = assemble(insns, 5, code);
    static shader_env e;
    memset(&e, 0, sizeof(e));
    e.in[0x2fc / 4] = 2; /* vertex id */
    e.cb[1] = (const uint8_t *)cb1;
    e.cb_size[1] = sizeof(cb1);
    uint64_t bad = 0;
    ASSERT_EQ(shader_run(code, n, &e, &bad), 0);
    ASSERT_EQ(e.out[0x90 / 4], 44); /* c1[2*4 + 4] */
}

/* Compute: words from deko3d's sinewave kernel (S2R, ISCADD, IADD.CC/.X, STG) plus an
 * LDG; the buffer address sits in c0[0x140] as in deko3d's driver buffer, and its low
 * half overflows so the store needs the carry. */
static uint8_t g_global[0x100];

static void *test_mem(uint64_t va, uint32_t len) {
    uint64_t base = 0x200000000ull;
    return va >= base && va + len <= base + sizeof(g_global) ? g_global + (va - base) : NULL;
}

TEST(shader_compute_global_store_and_load) {
    static uint32_t cb0[0x150 / 4];
    cb0[0x140 / 4] = 0xFFFFFFF0u; /* buffer at 0x1_FFFFFFF0 */
    cb0[0x144 / 4] = 0x1;
    const uint64_t insns[] = {
        0xf0c8000002570000ull,                                      /* s2r r0 = ctaid.x */
        0xf0c8000002170001ull,                                      /* s2r r1 = tid.x */
        0x5c18028000170000ull,                                      /* iscadd r0 = r0<<5 + r1 */
        OPC(0x3848) | PT_GUARD | D(0) | A(0) | imm20i(2),           /* shl r0 <<= 2 */
        0x4c10800005070000ull,                                      /* iadd.cc r0 += c0[0x140] */
        0x4c1008000517ff01ull,                                      /* iadd.x r1 = c0[0x144] */
        OPC(0x0100) | PT_GUARD | D(8) | ((uint64_t)0xC0FFEE << 20), /* mov32i r8 */
        0xeedc200000470008ull,                                      /* stg.e [r0+4] = r8 */
        0xeed4200000470009ull,                                      /* ldg.e r9 = [r0+4] */
        OPC(0xeff0) | PT_GUARD | D(9) | A(RZ) | (0x80ull << 20) | C(RZ), /* st a[0x80] = r9 */
        OPC(0xe300) | PT_GUARD,
    };
    uint64_t code[24];
    uint32_t n = assemble(insns, sizeof(insns) / sizeof(insns[0]), code);
    static shader_env e;
    memset(&e, 0, sizeof(e));
    memset(g_global, 0, sizeof(g_global));
    e.cb[0] = (const uint8_t *)cb0;
    e.cb_size[0] = sizeof(cb0);
    e.mem = test_mem;
    e.ctaid[0] = 1;
    e.tid[0] = 2; /* index 34: 0xFFFFFFF0 + 136 carries into the high word */
    uint64_t bad = 0;
    ASSERT_EQ(shader_run(code, n, &e, &bad), 0);
    uint32_t v;
    memcpy(&v, g_global + 0x78 + 4, 4); /* 0x2_00000078 + 4 */
    ASSERT_EQ(v, 0xC0FFEE);
    ASSERT_EQ(e.out[0x80 / 4], 0xC0FFEE);
}

/* The compiler multiplies 32-bit integers with three XMADs (words from deko3d's
 * tessellation shaders): r0 = r1 * r2 + r0 over 16-bit halves. */
static uint32_t run_xmad_sequence(uint32_t a, uint32_t b, uint32_t c) {
    const uint64_t insns[] = {
        0x5b00000000270100ull, /* xmad r0 = r1.lo * r2.lo + r0 */
        0x5b007fa800270102ull, /* xmad.mrg r2 = r1.lo * r2.hi + RZ, high half from r2.lo */
        0x5b30001800270100ull, /* xmad.psl.cbcc r0 = (r1.hi * r2.hi << 16) + r0 + (r2.lo << 16) */
        OPC(0xeff0) | PT_GUARD | D(0) | A(RZ) | (0x80ull << 20) | C(RZ),
        OPC(0xe300) | PT_GUARD,
    };
    uint64_t code[16];
    uint32_t n = assemble(insns, sizeof(insns) / sizeof(insns[0]), code);
    static shader_env e;
    memset(&e, 0, sizeof(e));
    e.r[0] = c;
    e.r[1] = a;
    e.r[2] = b;
    uint64_t bad = 0;
    if (shader_run(code, n, &e, &bad) != 0) {
        return 0xDEAD0000u;
    }
    return e.out[0x80 / 4];
}

TEST(shader_xmad_sequence_is_a_32bit_multiply_add) {
    ASSERT_EQ(run_xmad_sequence(3, 7, 1), 22);
    ASSERT_EQ(run_xmad_sequence(0x12345, 0x6789A, 0x100), 0x12345u * 0x6789Au + 0x100u);
    ASSERT_EQ(run_xmad_sequence(0xFFFFFFFFu, 0xFFFFFFFFu, 0), 1); /* -1 * -1 */
    ASSERT_EQ(run_xmad_sequence(0x80000001u, 0x7FFFFFFFu, 0x55), 0x80000001u * 0x7FFFFFFFu + 0x55u);
    ASSERT_EQ(run_xmad_sequence(0, 0xABCDEF01u, 9), 9);
}

TEST(shader_xmad_immediate_and_modes) {
    const uint64_t insns[] = {
        /* xmad r3 = r1.lo * 0x1234 + r2 (16-bit immediate at [35:20]) */
        OPC(0x3600) | PT_GUARD | D(3) | A(1) | (0x1234ull << 20) | C(2),
        /* xmad.chi r4 = r1.hi * r2.lo + (r2 >> 16): A high half [53], mode 2 at [52:50] */
        OPC(0x5b00) | PT_GUARD | D(4) | A(1) | B(2) | C(2) | (1ull << 53) | (2ull << 50),
        /* xmad.clo s16 r5 = (int16)r1.lo * r2.lo + (r2 & 0xffff) */
        OPC(0x5b00) | PT_GUARD | D(5) | A(1) | B(2) | C(2) | (1ull << 48) | (1ull << 50),
        OPC(0xeff0) | PT_GUARD | D(3) | A(RZ) | (0x80ull << 20) | (2ull << 47) | C(RZ),
        OPC(0xe300) | PT_GUARD,
    };
    uint64_t code[16];
    uint32_t n = assemble(insns, sizeof(insns) / sizeof(insns[0]), code);
    static shader_env e;
    memset(&e, 0, sizeof(e));
    e.r[1] = 0x0005FFFE; /* lo = -2 as s16, hi = 5 */
    e.r[2] = 0x00030007;
    uint64_t bad = 0;
    ASSERT_EQ(shader_run(code, n, &e, &bad), 0);
    ASSERT_EQ(e.out[0x80 / 4], 0xFFFEu * 0x1234u + 0x00030007u);
    ASSERT_EQ(e.out[0x84 / 4], 5u * 7u + 3u);
    ASSERT_EQ(e.out[0x88 / 4], (uint32_t)(-2 * 7 + 7));
}

TEST(shader_bfe_extracts_fields) {
    const uint64_t insns[] = {
        0x3800000081070101ull, /* bfe u32 r1 = (r1 >> 16) & 0xff: immediate 0x810 */
        /* bfe (signed) r2 = r3[23:16]: the field's top bit sign-extends */
        OPC(0x3800) | PT_GUARD | D(2) | A(3) | imm20i(0x810) | (1ull << 48),
        /* bfe u32 r4 = r3 >> 28, width 8 clipped at bit 31 */
        OPC(0x3800) | PT_GUARD | D(4) | A(3) | imm20i(0x81C),
        /* bfe u32 r5 = r3 with width 0 */
        OPC(0x3800) | PT_GUARD | D(5) | A(3) | imm20i(0x004),
        /* bfe u32 r6 = (r1 >> r7[7:0]) & ((1 << r7[15:8]) - 1): register form, r1 is 0xAB now */
        OPC(0x5c00) | PT_GUARD | D(6) | A(1) | B(7),
        OPC(0xeff0) | PT_GUARD | D(1) | A(RZ) | (0x80ull << 20) | (3ull << 47) | C(RZ),
        OPC(0xeff0) | PT_GUARD | D(5) | A(RZ) | (0x90ull << 20) | (1ull << 47) | C(RZ),
        OPC(0xe300) | PT_GUARD,
    };
    uint64_t code[16];
    uint32_t n = assemble(insns, sizeof(insns) / sizeof(insns[0]), code);
    static shader_env e;
    memset(&e, 0, sizeof(e));
    e.r[1] = 0x00AB1234;
    e.r[3] = 0xF0801234;
    e.r[7] = 0x0404; /* 4 bits from bit 4 */
    uint64_t bad = 0;
    ASSERT_EQ(shader_run(code, n, &e, &bad), 0);
    ASSERT_EQ(e.out[0x80 / 4], 0xAB);        /* r1 */
    ASSERT_EQ(e.out[0x84 / 4], 0xFFFFFF80u); /* r2 */
    ASSERT_EQ(e.out[0x88 / 4], 0xF0801234u); /* r3, untouched */
    ASSERT_EQ(e.out[0x8c / 4], 0xF);         /* r4 */
    ASSERT_EQ(e.out[0x90 / 4], 0);           /* r5 */
    ASSERT_EQ(e.out[0x94 / 4], 0xA);         /* r6 */
}

/* A tessellation control shader's shape (words from deko3d's tess_simple_tcsh):
 * the invocation id names the input vertex whose position it copies to its own
 * output, and invocation 0 writes a tessellation level into the patch space. */
TEST(shader_tessellation_attributes) {
    const uint64_t insns[] = {
        0xf0c8000001170000ull, /* s2r r0 = invocation_id */
        0xf0c8000001d70001ull, /* s2r r1 = invocation_info */
        0x384700000ff70102ull, /* lop and r2 = r1 & 0xff */
        0x3800000081070101ull, /* bfe u32 r1 = r1[23:16] */
        0x5b00000000270100ull, /* r0 = r1 * r2 + r0 */
        0x5b007fa800270102ull,
        0x5b30001800270100ull,
        0xefd0000000070004ull, /* isberd r4 = p[r0] */
        0xefd982000707ff00ull, /* ld b128 r0 = a[0x70] of vertex r4 */
        0xeff1ff800707ff00ull, /* st b128 a[0x70] = r0 (own output) */
        0xefd982000807ff00ull, /* ld b128 r0 = a[0x80] of vertex r4 */
        0xeff1ff800807ff00ull, /* st b128 a[0x80] = r0 */
        OPC(0x0100) | PT_GUARD | D(5) | ((uint64_t)0x40a00000 << 20), /* mov32i r5 = 5.0 */
        0xeff07f808100ff05ull, /* @p0 st p b32 a[0x10] = r5 */
        0xefd881012f07ff00ull, /* ld o b64 r0 = a[0x2f0] r2 (tess coord, as the TES reads it) */
        OPC(0xeff0) | PT_GUARD | D(0) | A(RZ) | (0xa0ull << 20) | (1ull << 47) | C(RZ),
        OPC(0xe300) | PT_GUARD,
    };
    uint64_t code[32];
    uint32_t n = assemble(insns, sizeof(insns) / sizeof(insns[0]), code);
    static uint32_t v0[SHADER_ATTR_WORDS], v1[SHADER_ATTR_WORDS], v2[SHADER_ATTR_WORDS];
    static uint32_t patch[SHADER_ATTR_WORDS];
    const uint32_t *verts[3] = {v0, v1, v2};
    for (uint32_t k = 0; k < SHADER_ATTR_WORDS; k++) {
        v0[k] = 0x1000 + k;
        v1[k] = 0x2000 + k;
        v2[k] = 0x3000 + k;
    }
    memset(patch, 0, sizeof(patch));
    for (uint32_t inv = 0; inv < 3; inv++) {
        static shader_env e;
        memset(&e, 0, sizeof(e));
        e.in_vertices = verts;
        e.num_in_vertices = 3;
        e.patch = patch;
        e.invocation_id = inv;
        e.p[0] = inv == 0; /* the shader's isetp on the invocation id */
        e.out[0x2f0 / 4] = 0x11;
        e.out[0x2f4 / 4] = 0x22;
        uint64_t bad = 0;
        ASSERT_EQ(shader_run(code, n, &e, &bad), 0);
        for (uint32_t k = 0; k < 4; k++) {
            ASSERT_EQ(e.out[0x70 / 4 + k], verts[inv][0x70 / 4 + k]);
            ASSERT_EQ(e.out[0x80 / 4 + k], verts[inv][0x80 / 4 + k]);
        }
        ASSERT_EQ(e.out[0xa0 / 4], 0x11);
        ASSERT_EQ(e.out[0xa4 / 4], 0x22);
    }
    ASSERT_EQ(patch[0x10 / 4], 0x40a00000);
    ASSERT_EQ(patch[0], 0);
}

TEST(shader_unknown_instruction_is_reported) {
    const uint64_t insns[] = {0xdeadbeefcaf00000ull | PT_GUARD, OPC(0xe300) | PT_GUARD};
    uint64_t code[8];
    uint32_t n = assemble(insns, 2, code);
    static shader_env e;
    memset(&e, 0, sizeof(e));
    uint64_t bad = 0;
    ASSERT_EQ(shader_run(code, n, &e, &bad), -1);
    ASSERT_TRUE(bad == (0xdeadbeefcaf00000ull | PT_GUARD));
}

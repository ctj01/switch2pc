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

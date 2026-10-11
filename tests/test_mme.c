/* The 3D engine's macro interpreter against hand-assembled macros. */
#include <string.h>

#include "gpu_mme.h"
#include "test.h"

/* Instruction fields (see gpu_mme.c). */
#define OP(o)       ((uint32_t)(o))
#define ASSIGN(a)   ((uint32_t)(a) << 4)
#define EXIT        (1u << 7)
#define DST(r)      ((uint32_t)(r) << 8)
#define SRCA(r)     ((uint32_t)(r) << 11)
#define SRCB(r)     ((uint32_t)(r) << 14)
#define IMM(i)      ((uint32_t)(i) << 14)
#define BF(s, n, d) (((uint32_t)(s) << 17) | ((uint32_t)(n) << 22) | ((uint32_t)(d) << 27))

enum { ALU = 0, ADDI = 1, INSERT = 2, READ = 5, BRANCH = 7 };
enum { FETCH = 0, MOVE = 1, MOVE_SET_METHOD = 2, FETCH_SEND = 3, MOVE_SEND = 4 };
#define BRANCH_NONZERO (1u << 4)

typedef struct sink {
    uint32_t n;
    uint32_t method[16];
    uint32_t value[16];
} sink;

static void on_send(void *user, uint32_t method, uint32_t value) {
    sink *s = user;
    if (s->n < 16) {
        s->method[s->n] = method;
        s->value[s->n] = value;
        s->n++;
    }
}

static uint32_t on_read(void *user, uint32_t method) {
    (void)user;
    return method * 2;
}

TEST(mme_send_fetch_and_exit_delay_slot) {
    const uint32_t code[] = {
        OP(ADDI) | ASSIGN(MOVE_SET_METHOD) | SRCA(0) | IMM(0x1123),       /* method 0x123, +1 */
        OP(ADDI) | ASSIGN(MOVE_SEND) | SRCA(1) | IMM(0),                  /* send param 1 */
        OP(ADDI) | ASSIGN(FETCH_SEND) | DST(2) | SRCA(1) | IMM(1) | EXIT, /* r2 = param 2 */
        OP(ADDI) | ASSIGN(MOVE_SEND) | SRCA(2) | IMM(0),  /* delay slot: still runs */
        OP(ADDI) | ASSIGN(MOVE_SEND) | SRCA(0) | IMM(99), /* must not run */
    };
    const uint32_t params[] = {10, 20};
    sink s = {0};
    ASSERT_EQ(mme_execute(code, 5, 0, params, 2, on_send, on_read, &s), 0);
    ASSERT_EQ(s.n, 3);
    ASSERT_EQ(s.method[0], 0x123);
    ASSERT_EQ(s.value[0], 10);
    ASSERT_EQ(s.method[1], 0x124);
    ASSERT_EQ(s.value[1], 11);
    ASSERT_EQ(s.method[2], 0x125);
    ASSERT_EQ(s.value[2], 20);
}

TEST(mme_loop_with_branch_delay_slot) {
    const uint32_t code[] = {
        OP(ADDI) | ASSIGN(MOVE_SET_METHOD) | SRCA(0) | IMM(0x1200),     /* 0 */
        OP(ADDI) | ASSIGN(MOVE_SEND) | DST(2) | SRCA(1) | IMM(0),       /* 1: send r1 */
        OP(ADDI) | ASSIGN(MOVE) | DST(1) | SRCA(1) | IMM(-1 & 0x3FFFF), /* 2: r1-- */
        OP(BRANCH) | BRANCH_NONZERO | SRCA(1) | IMM(-2 & 0x3FFFF),      /* 3: back to 1 */
        OP(ADDI) | ASSIGN(MOVE) | DST(3) | SRCA(3) | IMM(1),            /* 4: delay slot */
        OP(ADDI) | ASSIGN(MOVE_SEND) | SRCA(3) | IMM(0) | EXIT,         /* 5 */
        OP(ADDI) | ASSIGN(MOVE) | IMM(0),                               /* 6 */
    };
    const uint32_t params[] = {3};
    sink s = {0};
    ASSERT_EQ(mme_execute(code, 7, 0, params, 1, on_send, on_read, &s), 0);
    ASSERT_EQ(s.n, 4);
    ASSERT_EQ(s.value[0], 3);
    ASSERT_EQ(s.value[1], 2);
    ASSERT_EQ(s.value[2], 1);
    ASSERT_EQ(s.method[3], 0x203);
    ASSERT_EQ(s.value[3], 3); /* the delay slot ran on both taken branches and the fall-through */
}

TEST(mme_read_and_extract_insert) {
    const uint32_t code[] = {
        OP(READ) | ASSIGN(MOVE) | DST(2) | IMM(0x50), /* r2 = reg[0x50] */
        OP(INSERT) | ASSIGN(MOVE) | DST(3) | SRCA(2) | SRCB(1) |
            BF(0, 4, 8), /* r1[3:0] -> [11:8] */
        OP(ADDI) | ASSIGN(MOVE_SET_METHOD) | SRCA(0) | IMM(0x300),
        OP(ADDI) | ASSIGN(MOVE_SEND) | SRCA(3) | IMM(0) | EXIT,
        OP(ADDI) | ASSIGN(MOVE) | IMM(0),
    };
    const uint32_t params[] = {0xF};
    sink s = {0};
    ASSERT_EQ(mme_execute(code, 5, 0, params, 1, on_send, on_read, &s), 0);
    ASSERT_EQ(s.n, 1);
    ASSERT_EQ(s.method[0], 0x300);
    ASSERT_EQ(s.value[0], 0xFA0); /* 0xA0 from the read, 0xF inserted at bit 8 */
}

TEST(mme_alu_and_runaway) {
    const uint32_t code[] = {
        OP(ALU) | ASSIGN(MOVE) | DST(2) | SRCA(1) | SRCB(1) | (2u << 17), /* r2 = r1 - r1 */
        OP(ADDI) | ASSIGN(MOVE_SET_METHOD) | SRCA(0) | IMM(0x10),
        OP(ALU) | ASSIGN(MOVE_SEND) | SRCA(1) | SRCB(2) | (9u << 17) | EXIT, /* r1 | r2 */
        OP(ADDI) | ASSIGN(MOVE) | IMM(0),
    };
    const uint32_t params[] = {0x55};
    sink s = {0};
    ASSERT_EQ(mme_execute(code, 4, 0, params, 1, on_send, on_read, &s), 0);
    ASSERT_EQ(s.n, 1);
    ASSERT_EQ(s.value[0], 0x55);

    const uint32_t forever[] = {OP(BRANCH) | (1u << 5) | IMM(0)}; /* r0 == 0: jump to self */
    s.n = 0;
    ASSERT_EQ(mme_execute(forever, 1, 0, params, 1, on_send, on_read, &s), -1);
}

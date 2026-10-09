#include "patch/patch.h"
#include "test.h"

/* svc #0x0B, mrs x0 tpidrro_el0, mrs x3 cntpct_el0, add x0 x1 x2, svc #0x80, brk #0, svc #0,
 * mrs x5 cntvct_el0, mrs x9 cntfrq_el0, mrs xzr cntvct_el0 */
static const uint32_t k_text[] = {
    0xD4000161, 0xD53BD060, 0xD53BE023, 0x8B020020, 0xD4001001,
    0xD4200000, 0xD4000001, 0xD53BE045, 0xD53BE009, 0xD53BE05F,
};
static const uint32_t k_expected[] = {
    0xD4200160, 0xD53BD040, 0xD4202060, 0x8B020020, 0xD4001001,
    0xD4200000, 0xD4000001, 0xD42020A0, 0xD4202520, 0xD53BE05F,
};

TEST(patch_classify_shapes) {
    patch_site s;
    ASSERT_EQ(patch_classify(0xD4000161, &s), 1);
    ASSERT_EQ(s.kind, PATCH_SVC_TO_BRK);
    ASSERT_EQ(s.imm, 0x0B);
    ASSERT_EQ(s.after, 0xD4200160);

    ASSERT_EQ(patch_classify(0xD53BD07E, &s), 1); /* mrs x30, tpidrro_el0 */
    ASSERT_EQ(s.kind, PATCH_TPIDRRO_TO_TPIDR);
    ASSERT_EQ(s.after, 0xD53BD05E);

    ASSERT_EQ(patch_classify(0xD53BE020, &s), 1); /* mrs x0, cntpct_el0 -> brk #0x100 */
    ASSERT_EQ(s.kind, PATCH_COUNTER_TO_BRK);
    ASSERT_EQ(s.after, 0xD4200000 | (0x100 << 5));

    ASSERT_EQ(patch_classify(0xD53BE041, &s), 1); /* mrs x1, cntvct_el0 -> brk #0x101 */
    ASSERT_EQ(s.kind, PATCH_COUNTER_TO_BRK);
    ASSERT_EQ(s.after, 0xD4200000 | (0x101 << 5));

    ASSERT_EQ(patch_classify(0xD53BE002, &s), 1); /* mrs x2, cntfrq_el0 -> brk #0x122 */
    ASSERT_EQ(s.kind, PATCH_CNTFRQ_TO_BRK);
    ASSERT_EQ(s.after, 0xD4200000 | (0x122 << 5));

    ASSERT_EQ(patch_classify(0xD53BD040, &s), 0); /* mrs x0, tpidr_el0: already fine */
    ASSERT_EQ(patch_classify(0xD53BE05F, &s), 0); /* mrs xzr, cntvct_el0: no-op */
    ASSERT_EQ(patch_classify(0xD4000001, &s), 0); /* svc #0 */
    ASSERT_EQ(patch_classify(0xD4001001, &s), 0); /* svc #0x80 */
    ASSERT_EQ(patch_classify(0xD4200000, &s), 0); /* brk #0 */
    ASSERT_EQ(patch_classify(0xD65F03C0, &s), 0); /* ret */
}

static void count_visit(const patch_site *s, void *user) {
    size_t *n = user;
    uint64_t expected_off[] = {0, 4, 8, 28, 32};
    if (*n < 5 && s->offset != expected_off[*n]) {
        (*n) = 100;
        return;
    }
    (*n)++;
}

TEST(patch_dry_run_leaves_text_alone) {
    uint32_t text[sizeof(k_text) / 4];
    memcpy(text, k_text, sizeof(k_text));
    patch_stats st;
    size_t visited = 0;
    ASSERT_EQ(patch_text((uint8_t *)text, sizeof(text), 0, &st, count_visit, &visited),
              HOS_RESULT_SUCCESS);
    ASSERT_EQ(st.total, 5);
    ASSERT_EQ(st.by_kind[PATCH_SVC_TO_BRK], 1);
    ASSERT_EQ(st.by_kind[PATCH_TPIDRRO_TO_TPIDR], 1);
    ASSERT_EQ(st.by_kind[PATCH_COUNTER_TO_BRK], 2);
    ASSERT_EQ(st.by_kind[PATCH_CNTFRQ_TO_BRK], 1);
    ASSERT_EQ(visited, 5);
    ASSERT_MEM_EQ(text, k_text, sizeof(k_text));
}

TEST(patch_apply_rewrites_in_place) {
    uint32_t text[sizeof(k_text) / 4];
    memcpy(text, k_text, sizeof(k_text));
    patch_stats st;
    ASSERT_EQ(patch_text((uint8_t *)text, sizeof(text), 1, &st, NULL, NULL), HOS_RESULT_SUCCESS);
    ASSERT_EQ(st.total, 5);
    ASSERT_MEM_EQ(text, k_expected, sizeof(k_expected));

    /* Second pass finds nothing: patching is idempotent. */
    ASSERT_EQ(patch_text((uint8_t *)text, sizeof(text), 1, &st, NULL, NULL), HOS_RESULT_SUCCESS);
    ASSERT_EQ(st.total, 0);
}

TEST(patch_ignores_trailing_bytes) {
    uint8_t text[6] = {0x61, 0x01, 0x00, 0xD4, 0xAA, 0xBB};
    patch_stats st;
    patch_text(text, sizeof(text), 1, &st, NULL, NULL);
    ASSERT_EQ(st.total, 1);
    ASSERT_EQ(text[4], 0xAA);
    ASSERT_EQ(text[5], 0xBB);
}

#ifndef S2PC_TEST_H
#define S2PC_TEST_H

/*
 * Minimal test harness. Tests register themselves through a constructor;
 * the framework provides main(). A failed assertion ends the current test.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct test_case {
    const char *name;
    void (*fn)(void);
    struct test_case *next;
} test_case;

void test_register(test_case *tc);
void test_fail(const char *file, int line, const char *msg);
int test_run_all(void);

#define TEST(name)                                                                                 \
    static void test_fn_##name(void);                                                              \
    static test_case test_case_##name = {#name, test_fn_##name, NULL};                             \
    __attribute__((constructor)) static void test_reg_##name(void) {                               \
        test_register(&test_case_##name);                                                          \
    }                                                                                              \
    static void test_fn_##name(void)

#define TEST_FAIL(msg)                                                                             \
    do {                                                                                           \
        test_fail(__FILE__, __LINE__, msg);                                                        \
        return;                                                                                    \
    } while (0)

#define ASSERT_TRUE(cond)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            TEST_FAIL("expected: " #cond);                                                         \
        }                                                                                          \
    } while (0)

#define ASSERT_EQ(a, b)                                                                            \
    do {                                                                                           \
        long long test_a_ = (long long)(a);                                                        \
        long long test_b_ = (long long)(b);                                                        \
        if (test_a_ != test_b_) {                                                                  \
            char test_m_[256];                                                                     \
            snprintf(test_m_, sizeof(test_m_), "%s == %s: %lld (%#llx) != %lld (%#llx)", #a, #b,   \
                     test_a_, (unsigned long long)test_a_, test_b_, (unsigned long long)test_b_);  \
            TEST_FAIL(test_m_);                                                                    \
        }                                                                                          \
    } while (0)

#define ASSERT_NE(a, b)                                                                            \
    do {                                                                                           \
        if ((long long)(a) == (long long)(b)) {                                                    \
            TEST_FAIL("expected " #a " != " #b);                                                   \
        }                                                                                          \
    } while (0)

#define ASSERT_STR_EQ(a, b)                                                                        \
    do {                                                                                           \
        const char *test_a_ = (a);                                                                 \
        const char *test_b_ = (b);                                                                 \
        if (strcmp(test_a_, test_b_) != 0) {                                                       \
            char test_m_[512];                                                                     \
            snprintf(test_m_, sizeof(test_m_), "%s: \"%s\" != \"%s\"", #a, test_a_, test_b_);      \
            TEST_FAIL(test_m_);                                                                    \
        }                                                                                          \
    } while (0)

#define ASSERT_MEM_EQ(a, b, n)                                                                     \
    do {                                                                                           \
        if (memcmp((a), (b), (n)) != 0) {                                                          \
            TEST_FAIL("memory differs: " #a " vs " #b);                                            \
        }                                                                                          \
    } while (0)

#endif /* S2PC_TEST_H */

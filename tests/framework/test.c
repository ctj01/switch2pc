#include "test.h"

#include <stdlib.h>

static test_case *g_head;
static test_case *g_tail;
static int g_current_failed;
static const char *g_current_name;

void test_register(test_case *tc) {
    tc->next = NULL;
    if (g_tail) {
        g_tail->next = tc;
    } else {
        g_head = tc;
    }
    g_tail = tc;
}

void test_fail(const char *file, int line, const char *msg) {
    fprintf(stderr, "  FAIL %s  %s:%d: %s\n", g_current_name, file, line, msg);
    g_current_failed = 1;
}

int test_run_all(void) {
    int total = 0;
    int failed = 0;
    for (test_case *tc = g_head; tc; tc = tc->next) {
        g_current_name = tc->name;
        g_current_failed = 0;
        tc->fn();
        total++;
        if (g_current_failed) {
            failed++;
        } else {
            fprintf(stderr, "  ok   %s\n", tc->name);
        }
    }
    fprintf(stderr, "%d tests, %d failed\n", total, failed);
    return failed ? 1 : 0;
}

int main(void) {
    return test_run_all();
}

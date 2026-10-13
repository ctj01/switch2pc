/*
 * Vsync pacing (#26): the 60 Hz grid is absolute, HOS_VSYNC parses as
 * documented, and thirty paced presents take at least half a second.
 */

#include <stdlib.h>

#include "test.h"
#include "vsync.h"

TEST(vsync_parse_env) {
    ASSERT_TRUE(vsync_parse(NULL));
    ASSERT_TRUE(vsync_parse(""));
    ASSERT_TRUE(vsync_parse("1"));
    ASSERT_TRUE(vsync_parse("auto"));
    ASSERT_TRUE(!vsync_parse("0"));
    ASSERT_TRUE(!vsync_parse("off"));
    ASSERT_TRUE(!vsync_parse("no"));
}

TEST(vsync_grid_is_absolute) {
    uint64_t now = vsync_now_ns();
    uint64_t b = vsync_next_boundary(now);
    ASSERT_TRUE(b > now);
    ASSERT_TRUE(b - now <= VSYNC_PERIOD_NS);
    /* Asking from a boundary gives the next one; from just before it, itself. */
    ASSERT_EQ(vsync_next_boundary(b), b + VSYNC_PERIOD_NS);
    ASSERT_EQ(vsync_next_boundary(b - 1), b);
    /* A long stall lands on the grid, never on "now + period". */
    ASSERT_EQ(vsync_next_boundary(b + 5 * VSYNC_PERIOD_NS + 3), b + 6 * VSYNC_PERIOD_NS);
}

TEST(vsync_thirty_frames_take_half_a_second) {
    enum { FRAMES = 30 };
    uint64_t t0 = vsync_now_ns();
    uint64_t prev = 0;
    for (int i = 0; i < FRAMES; i++) {
        uint64_t b = vsync_wait_next();
        ASSERT_TRUE(vsync_now_ns() >= b); /* woke at or after the boundary */
        if (prev) {
            ASSERT_TRUE(b > prev);
            ASSERT_EQ((b - prev) % VSYNC_PERIOD_NS, 0); /* stays on the grid */
        }
        prev = b;
    }
    uint64_t elapsed = vsync_now_ns() - t0;
    /* 29 full periods after the first partial one: 483 ms at least. */
    ASSERT_TRUE(elapsed >= 450000000ull);
    ASSERT_TRUE(elapsed < 10000000000ull);
}

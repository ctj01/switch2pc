/* Vsync pacing: see vsync.h. */

#include "vsync.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_epoch; /* the first refresh; 0 until the grid is anchored */
static int g_enabled = -1;

int vsync_parse(const char *value) {
    if (!value || !*value) {
        return 1;
    }
    return !(strcmp(value, "0") == 0 || strcmp(value, "off") == 0 || strcmp(value, "no") == 0);
}

int vsync_enabled(void) {
    pthread_mutex_lock(&g_lock);
    if (g_enabled < 0) {
        g_enabled = vsync_parse(getenv("HOS_VSYNC"));
    }
    int enabled = g_enabled;
    pthread_mutex_unlock(&g_lock);
    return enabled;
}

uint64_t vsync_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t vsync_next_boundary(uint64_t now_ns) {
    pthread_mutex_lock(&g_lock);
    if (!g_epoch) {
        g_epoch = now_ns; /* anchored once: a second thread's earlier `now` keeps it */
    }
    uint64_t boundary = g_epoch;
    if (now_ns >= g_epoch) {
        boundary = g_epoch + ((now_ns - g_epoch) / VSYNC_PERIOD_NS + 1) * VSYNC_PERIOD_NS;
    }
    pthread_mutex_unlock(&g_lock);
    return boundary;
}

uint64_t vsync_hold_until(uint64_t shown_at, uint64_t now) {
    return shown_at > now ? shown_at : 0;
}

uint64_t vsync_advance(uint64_t shown_at, uint64_t now) {
    uint64_t next = vsync_next_boundary(now);
    return shown_at + VSYNC_PERIOD_NS > next ? shown_at + VSYNC_PERIOD_NS : next;
}

void vsync_sleep_until(uint64_t deadline_ns) {
    struct timespec ts = {.tv_sec = (time_t)(deadline_ns / 1000000000ull),
                          .tv_nsec = (long)(deadline_ns % 1000000000ull)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {
    }
}

uint64_t vsync_wait_next(void) {
    uint64_t boundary = vsync_next_boundary(vsync_now_ns());
    vsync_sleep_until(boundary);
    return boundary;
}

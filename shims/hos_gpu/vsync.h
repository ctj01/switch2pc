#ifndef HOS_GPU_VSYNC_H
#define HOS_GPU_VSYNC_H

#include <stdint.h>

/*
 * Vsync pacing (#26). The console's compositor shows one frame per 60 Hz
 * refresh, so a game that presents faster than that is held in QueueBuffer
 * until the next refresh and runs at 60 fps; a slower one lands on the
 * refresh after it is done. Refreshes are an absolute grid on the monotonic
 * clock, anchored at first use, so a late wake never moves the next one and
 * the vsync event (vi 5202) fires on the same grid. HOS_VSYNC=0 turns the
 * pacing off; the tests, scripts/action_run.sh and the benchmarks run that way.
 */

#define VSYNC_PERIOD_NS 16666667ull

/** 1 unless the value says off ("0", "off", "no"); NULL and "" mean on. */
int vsync_parse(const char *value);

/** Whether pacing is on for this process (HOS_VSYNC, read once). */
int vsync_enabled(void);

/** CLOCK_MONOTONIC in nanoseconds. */
uint64_t vsync_now_ns(void);

/** The first refresh strictly after `now_ns` on the grid (anchored on first call). */
uint64_t vsync_next_boundary(uint64_t now_ns);

/** Sleep until the absolute monotonic time `deadline_ns`, through signals. */
void vsync_sleep_until(uint64_t deadline_ns);

/** Block until the next refresh after now; returns its time. */
uint64_t vsync_wait_next(void);

#endif /* HOS_GPU_VSYNC_H */

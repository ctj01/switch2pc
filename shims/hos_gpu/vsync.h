#ifndef HOS_GPU_VSYNC_H
#define HOS_GPU_VSYNC_H

#include <stdint.h>

/*
 * Vsync pacing (#26). The console's compositor shows one frame per 60 Hz
 * refresh and a layer's queue is one frame deep: QueueBuffer only blocks while
 * the previous frame is still waiting for its refresh. A game that presents
 * faster than 60 Hz is held until that refresh and runs at 60 fps; a slower one
 * is never held, and its frame goes up on the refresh after it is queued.
 * Refreshes are an absolute grid on the monotonic clock, anchored at first
 * use, so a late wake never moves the next one and the vsync event (vi 5202)
 * fires on the same grid. HOS_VSYNC=0 turns the pacing off; the tests,
 * scripts/action_run.sh and the benchmarks run that way.
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

/*
 * The one-deep queue of a layer: `shown_at` is the refresh at which its
 * previous frame goes up (0 before the first). A frame queued at `now` holds
 * the presenter until that refresh, if it is still to come, and then takes the
 * refresh after it, or the next one on the grid when the game is slower.
 */
/** When to hold the presenter until for a frame queued at `now`; 0 for no hold. */
uint64_t vsync_hold_until(uint64_t shown_at, uint64_t now);
/** The refresh the frame queued at `now` (after any hold) goes up at. */
uint64_t vsync_advance(uint64_t shown_at, uint64_t now);

#endif /* HOS_GPU_VSYNC_H */

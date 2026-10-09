#ifndef HOS_INPUT_STATE_H
#define HOS_INPUT_STATE_H

/*
 * Controller state into the hid shared memory. A writer thread refreshes the
 * Npad LIFO at 60 Hz from whichever sources are configured:
 *
 *   HOS_INPUT_SCRIPT=<file>   lines "<ms> <buttons hex> [lx ly rx ry]"; each line
 *                             becomes the state from that time on (sticks in
 *                             -32767..32767, Switch button bit numbering)
 *   HOS_INPUT_EVDEV=<path>    a Linux evdev gamepad (/dev/input/eventN), or
 *                             "auto" to take the first device with BTN_GAMEPAD
 *
 * Player 1 is reported as a wired Pro Controller.
 */

#include <stdint.h>

typedef struct hos_input_state {
    uint64_t buttons;
    int32_t lx, ly, rx, ry;
} hos_input_state;

/**
 * Start the writer thread on a freshly created hid shared memory view.
 * Safe to call once; later calls are ignored.
 */
void hos_input_start(void *shmem_view);

#endif /* HOS_INPUT_STATE_H */

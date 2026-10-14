#ifndef HOS_GPU_DISPLAY_X11_H
#define HOS_GPU_DISPLAY_X11_H

#include <stdint.h>

/*
 * X11 window for the vi null compositor (#25). Opt-in: HOS_WINDOW=1 (or "x11")
 * with DISPLAY set, so tests and headless runs never open a window. The first
 * presented frame opens a window of frame size x HOS_WINDOW_SCALE (default 2);
 * frames are scaled with nearest-neighbour into whatever size the window has,
 * letterboxed to keep the aspect ratio. The window owns the keyboard and feeds
 * player 1 through hos_pad_external_set; closing it sends the applet exit
 * request so the game leaves through its own quit path.
 *
 * Keys: arrows = D-pad, Z/Ctrl = A, X/Space = B, C = X, V = Y, Q = L, E = R,
 * Enter = Plus, Esc = Minus, WASD = left stick.
 */

/** Whether HOS_WINDOW asks for a window (so frames are worth decoding for it). */
int display_x11_wanted(void);

/** Present one 8-bit RGB frame. Opens the window on first use; returns 0 if shown. */
int display_x11_present(const uint8_t *rgb, uint32_t width, uint32_t height);

/** Close the window, if any. */
void display_x11_quit(void);

#endif /* HOS_GPU_DISPLAY_X11_H */

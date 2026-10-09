/*
 * switch2pc software video driver for SDL2 on libnx. Replaces the EGL/GLES
 * driver from the devkitPro fork: windows are plain libnx framebuffers and
 * the SDL software renderer draws into them. No GPU is needed.
 *
 * Same license as SDL (zlib). Overlaid onto src/video/switch/ by
 * scripts/build_sdl2.sh.
 */
#ifndef __SDL_SWITCHVIDEO_H__
#define __SDL_SWITCHVIDEO_H__

#if SDL_VIDEO_DRIVER_SWITCH

#include <switch.h>

#include "../../SDL_internal.h"
#include "../SDL_sysvideo.h"

typedef struct SDL_WindowData
{
    Framebuffer fb;
    SDL_bool fb_ready;
    SDL_Surface *surface; /* SDL-side framebuffer handed to the software renderer */
} SDL_WindowData;

int SWITCH_VideoInit(_THIS);
void SWITCH_VideoQuit(_THIS);
void SWITCH_GetDisplayModes(_THIS, SDL_VideoDisplay *display);
int SWITCH_SetDisplayMode(_THIS, SDL_VideoDisplay *display, SDL_DisplayMode *mode);
int SWITCH_CreateWindow(_THIS, SDL_Window *window);
int SWITCH_CreateWindowFrom(_THIS, SDL_Window *window, const void *data);
void SWITCH_SetWindowTitle(_THIS, SDL_Window *window);
void SWITCH_SetWindowIcon(_THIS, SDL_Window *window, SDL_Surface *icon);
void SWITCH_SetWindowPosition(_THIS, SDL_Window *window);
void SWITCH_SetWindowSize(_THIS, SDL_Window *window);
void SWITCH_ShowWindow(_THIS, SDL_Window *window);
void SWITCH_HideWindow(_THIS, SDL_Window *window);
void SWITCH_RaiseWindow(_THIS, SDL_Window *window);
void SWITCH_MaximizeWindow(_THIS, SDL_Window *window);
void SWITCH_MinimizeWindow(_THIS, SDL_Window *window);
void SWITCH_RestoreWindow(_THIS, SDL_Window *window);
void SWITCH_DestroyWindow(_THIS, SDL_Window *window);
int SWITCH_CreateWindowFramebuffer(_THIS, SDL_Window *window, Uint32 *format, void **pixels,
                                   int *pitch);
int SWITCH_UpdateWindowFramebuffer(_THIS, SDL_Window *window, const SDL_Rect *rects,
                                   int numrects);
void SWITCH_DestroyWindowFramebuffer(_THIS, SDL_Window *window);
void SWITCH_PumpEvents(_THIS);

#endif /* SDL_VIDEO_DRIVER_SWITCH */

#endif /* __SDL_SWITCHVIDEO_H__ */

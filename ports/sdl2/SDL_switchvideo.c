/*
 * switch2pc software video driver for SDL2 on libnx. The window is a libnx
 * framebuffer; SDL's software renderer draws into an SDL surface that
 * UpdateWindowFramebuffer copies into the next libnx buffer. Keyboard, mouse,
 * touch and the software keyboard helpers from the devkitPro driver are kept.
 *
 * Same license as SDL (zlib). Overlaid onto src/video/switch/ by
 * scripts/build_sdl2.sh.
 */
#include "../../SDL_internal.h"

#if SDL_VIDEO_DRIVER_SWITCH

#include "../SDL_sysvideo.h"
#include "../../events/SDL_events_c.h"
#include "../../events/SDL_keyboard_c.h"
#include "../../events/SDL_mouse_c.h"

#include "SDL_switchvideo.h"
#include "SDL_switchtouch.h"
#include "SDL_switchkeyboard.h"
#include "SDL_switchmouse_c.h"
#include "SDL_switchswkb.h"

#include <switch.h>

static SDL_Window *switch_window = NULL;

static void SWITCH_Destroy(SDL_VideoDevice *device)
{
    if (device != NULL) {
        if (device->driverdata != NULL) {
            SDL_free(device->driverdata);
        }
        SDL_free(device);
    }
}

static SDL_VideoDevice *SWITCH_CreateDevice(void)
{
    SDL_VideoDevice *device = (SDL_VideoDevice *)SDL_calloc(1, sizeof(SDL_VideoDevice));
    if (device == NULL) {
        SDL_OutOfMemory();
        return NULL;
    }
    device->num_displays = 0;
    device->free = SWITCH_Destroy;

    device->VideoInit = SWITCH_VideoInit;
    device->VideoQuit = SWITCH_VideoQuit;
    device->GetDisplayModes = SWITCH_GetDisplayModes;
    device->SetDisplayMode = SWITCH_SetDisplayMode;
    device->CreateSDLWindow = SWITCH_CreateWindow;
    device->CreateSDLWindowFrom = SWITCH_CreateWindowFrom;
    device->SetWindowTitle = SWITCH_SetWindowTitle;
    device->SetWindowIcon = SWITCH_SetWindowIcon;
    device->SetWindowPosition = SWITCH_SetWindowPosition;
    device->SetWindowSize = SWITCH_SetWindowSize;
    device->ShowWindow = SWITCH_ShowWindow;
    device->HideWindow = SWITCH_HideWindow;
    device->RaiseWindow = SWITCH_RaiseWindow;
    device->MaximizeWindow = SWITCH_MaximizeWindow;
    device->MinimizeWindow = SWITCH_MinimizeWindow;
    device->RestoreWindow = SWITCH_RestoreWindow;
    device->DestroyWindow = SWITCH_DestroyWindow;
    device->CreateWindowFramebuffer = SWITCH_CreateWindowFramebuffer;
    device->UpdateWindowFramebuffer = SWITCH_UpdateWindowFramebuffer;
    device->DestroyWindowFramebuffer = SWITCH_DestroyWindowFramebuffer;

    device->StartTextInput = SWITCH_StartTextInput;
    device->StopTextInput = SWITCH_StopTextInput;
    device->HasScreenKeyboardSupport = SWITCH_HasScreenKeyboardSupport;
    device->IsScreenKeyboardShown = SWITCH_IsScreenKeyboardShown;

    device->PumpEvents = SWITCH_PumpEvents;
    return device;
}

VideoBootStrap SWITCH_bootstrap = {
    "Switch",
    "switch2pc software framebuffer",
    SWITCH_CreateDevice
};

int SWITCH_VideoInit(_THIS)
{
    SDL_VideoDisplay display;
    SDL_DisplayMode current_mode;

    SDL_zero(current_mode);
    current_mode.w = 1280;
    current_mode.h = 720;
    current_mode.refresh_rate = 60;
    current_mode.format = SDL_PIXELFORMAT_ABGR8888;
    current_mode.driverdata = NULL;

    SDL_zero(display);
    display.desktop_mode = current_mode;
    display.current_mode = current_mode;
    display.driverdata = NULL;
    SDL_AddVideoDisplay(&display, SDL_FALSE);

    SWITCH_InitTouch();
    SWITCH_InitKeyboard();
    SWITCH_InitMouse();
    SWITCH_InitSwkb();
    return 0;
}

void SWITCH_VideoQuit(_THIS)
{
    SWITCH_QuitTouch();
    SWITCH_QuitKeyboard();
    SWITCH_QuitMouse();
    SWITCH_QuitSwkb();
}

void SWITCH_GetDisplayModes(_THIS, SDL_VideoDisplay *display)
{
    SDL_AddDisplayMode(display, &display->current_mode);
}

int SWITCH_SetDisplayMode(_THIS, SDL_VideoDisplay *display, SDL_DisplayMode *mode)
{
    return 0;
}

int SWITCH_CreateWindow(_THIS, SDL_Window *window)
{
    SDL_WindowData *data;

    if (switch_window != NULL) {
        return SDL_SetError("Switch only supports one window");
    }
    data = (SDL_WindowData *)SDL_calloc(1, sizeof(SDL_WindowData));
    if (data == NULL) {
        return SDL_OutOfMemory();
    }
    /* The compositor scales whatever size the app asked for; no fullscreen games. */
    window->driverdata = data;
    window->flags |= SDL_WINDOW_SHOWN | SDL_WINDOW_INPUT_FOCUS;
    window->flags &= ~(SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP);
    switch_window = window;

    SDL_SetMouseFocus(window);
    SDL_SetKeyboardFocus(window);
    return 0;
}

int SWITCH_CreateWindowFrom(_THIS, SDL_Window *window, const void *data)
{
    return SDL_Unsupported();
}

void SWITCH_SetWindowTitle(_THIS, SDL_Window *window)
{
}
void SWITCH_SetWindowIcon(_THIS, SDL_Window *window, SDL_Surface *icon)
{
}
void SWITCH_SetWindowPosition(_THIS, SDL_Window *window)
{
}
void SWITCH_SetWindowSize(_THIS, SDL_Window *window)
{
}
void SWITCH_ShowWindow(_THIS, SDL_Window *window)
{
}
void SWITCH_HideWindow(_THIS, SDL_Window *window)
{
}
void SWITCH_RaiseWindow(_THIS, SDL_Window *window)
{
}
void SWITCH_MaximizeWindow(_THIS, SDL_Window *window)
{
}
void SWITCH_MinimizeWindow(_THIS, SDL_Window *window)
{
}
void SWITCH_RestoreWindow(_THIS, SDL_Window *window)
{
}

void SWITCH_DestroyWindow(_THIS, SDL_Window *window)
{
    SDL_WindowData *data = (SDL_WindowData *)window->driverdata;
    if (data != NULL) {
        SWITCH_DestroyWindowFramebuffer(_this, window);
        SDL_free(data);
        window->driverdata = NULL;
    }
    switch_window = NULL;
}

int SWITCH_CreateWindowFramebuffer(_THIS, SDL_Window *window, Uint32 *format, void **pixels,
                                   int *pitch)
{
    SDL_WindowData *data = (SDL_WindowData *)window->driverdata;
    Result rc;
    int w, h;

    SWITCH_DestroyWindowFramebuffer(_this, window);
    SDL_GetWindowSizeInPixels(window, &w, &h);

    /* RGBA8888 in memory is ABGR8888 as SDL names little-endian formats. */
    rc = framebufferCreate(&data->fb, nwindowGetDefault(), (u32)w, (u32)h,
                           PIXEL_FORMAT_RGBA_8888, 2);
    if (R_FAILED(rc)) {
        return SDL_SetError("framebufferCreate failed: 0x%x", rc);
    }
    framebufferMakeLinear(&data->fb);
    data->fb_ready = SDL_TRUE;

    data->surface = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ABGR8888);
    if (data->surface == NULL) {
        return -1;
    }
    *format = SDL_PIXELFORMAT_ABGR8888;
    *pixels = data->surface->pixels;
    *pitch = data->surface->pitch;
    return 0;
}

int SWITCH_UpdateWindowFramebuffer(_THIS, SDL_Window *window, const SDL_Rect *rects,
                                   int numrects)
{
    SDL_WindowData *data = (SDL_WindowData *)window->driverdata;
    u32 stride = 0;
    u8 *dst;
    int y;

    if (data == NULL || !data->fb_ready || data->surface == NULL) {
        return SDL_SetError("no framebuffer");
    }
    dst = (u8 *)framebufferBegin(&data->fb, &stride);
    if (dst == NULL) {
        return SDL_SetError("framebufferBegin failed");
    }
    for (y = 0; y < data->surface->h; y++) {
        SDL_memcpy(dst + (size_t)y * stride,
                   (const u8 *)data->surface->pixels + (size_t)y * data->surface->pitch,
                   (size_t)data->surface->w * 4);
    }
    framebufferEnd(&data->fb);
    return 0;
}

void SWITCH_DestroyWindowFramebuffer(_THIS, SDL_Window *window)
{
    SDL_WindowData *data = (SDL_WindowData *)window->driverdata;
    if (data == NULL) {
        return;
    }
    if (data->surface != NULL) {
        SDL_FreeSurface(data->surface);
        data->surface = NULL;
    }
    if (data->fb_ready) {
        framebufferClose(&data->fb);
        data->fb_ready = SDL_FALSE;
    }
}

void SWITCH_PumpEvents(_THIS)
{
    if (!appletMainLoop()) {
        SDL_Event ev;
        ev.type = SDL_QUIT;
        SDL_PushEvent(&ev);
        return;
    }
    if (!SDL_IsTextInputActive()) {
        SWITCH_PollTouch();
        SWITCH_PollKeyboard();
        SWITCH_PollMouse();
    }
    SWITCH_PollSwkb();
}

#endif /* SDL_VIDEO_DRIVER_SWITCH */

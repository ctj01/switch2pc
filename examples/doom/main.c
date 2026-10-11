/*
 * DOOM on switch2pc: doomgeneric's platform layer for libnx. The engine
 * renders into DG_ScreenBuffer with the CPU; we copy it into a libnx
 * framebuffer (which hosrun's vi consumes and can dump), read the pad from
 * the hid shared memory, and time with the system tick.
 *
 * Build: scripts/build_doom.sh (needs third_party/doomgeneric and
 * examples/doom/romfs/doom1.wad, the shareware IWAD).
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "doomgeneric.h"
#include "doomkeys.h"

#ifndef DOOM_FRAME_LIMIT
#define DOOM_FRAME_LIMIT 400 /* headless runs stop here; 0 = never */
#endif

static void say(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        svcOutputDebugString(buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
    }
}

static Framebuffer g_fb;
static PadState g_pad;
static u64 g_prev_buttons;
static unsigned g_frames;

#define KEY_QUEUE 64
static struct {
    unsigned char key;
    int pressed;
} g_queue[KEY_QUEUE];
static unsigned g_queue_head, g_queue_tail;

static const struct {
    u64 buttons;
    unsigned char key;
} k_map[] = {
    {HidNpadButton_A | HidNpadButton_ZR, KEY_FIRE},
    {HidNpadButton_B | HidNpadButton_ZL, KEY_USE},
    {HidNpadButton_X, KEY_TAB},
    {HidNpadButton_Y, KEY_RSHIFT},
    {HidNpadButton_Plus, KEY_ENTER},
    {HidNpadButton_Minus, KEY_ESCAPE},
    {HidNpadButton_AnyUp, KEY_UPARROW},
    {HidNpadButton_AnyDown, KEY_DOWNARROW},
    {HidNpadButton_AnyLeft, KEY_LEFTARROW},
    {HidNpadButton_AnyRight, KEY_RIGHTARROW},
    {HidNpadButton_L, KEY_STRAFE_L},
    {HidNpadButton_R, KEY_STRAFE_R},
};

static void queue_key(unsigned char key, int pressed) {
    unsigned next = (g_queue_tail + 1) % KEY_QUEUE;
    if (next == g_queue_head) {
        return; /* full: drop */
    }
    g_queue[g_queue_tail].key = key;
    g_queue[g_queue_tail].pressed = pressed;
    g_queue_tail = next;
}

static void poll_input(void) {
    padUpdate(&g_pad);
    u64 now = padGetButtons(&g_pad);
    u64 changed = now ^ g_prev_buttons;
    if (!changed) {
        return;
    }
    for (size_t i = 0; i < sizeof(k_map) / sizeof(k_map[0]); i++) {
        int was = (g_prev_buttons & k_map[i].buttons) != 0;
        int is = (now & k_map[i].buttons) != 0;
        if (was != is) {
            queue_key(k_map[i].key, is);
        }
    }
    g_prev_buttons = now;
}

void DG_Init(void) {
    Result rc = framebufferCreate(&g_fb, nwindowGetDefault(), DOOMGENERIC_RESX, DOOMGENERIC_RESY,
                                  PIXEL_FORMAT_BGRA_8888, 2);
    if (R_FAILED(rc)) {
        say("FAIL framebufferCreate rc=%#x", rc);
        exit(1);
    }
    framebufferMakeLinear(&g_fb);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    say("doom: framebuffer %ux%u ready", DOOMGENERIC_RESX, DOOMGENERIC_RESY);
}

void DG_DrawFrame(void) {
    u32 stride = 0;
    u8 *fb = framebufferBegin(&g_fb, &stride);
    if (fb) {
        for (unsigned y = 0; y < DOOMGENERIC_RESY; y++) {
            memcpy(fb + (size_t)y * stride, DG_ScreenBuffer + (size_t)y * DOOMGENERIC_RESX,
                   DOOMGENERIC_RESX * sizeof(pixel_t));
        }
        framebufferEnd(&g_fb);
    }
    g_frames++;
    if (g_frames % 50 == 0) {
        say("doom: frame %u", g_frames);
    }
    if (DOOM_FRAME_LIMIT && g_frames >= DOOM_FRAME_LIMIT) {
        say("doom: done after %u frames", g_frames);
        framebufferClose(&g_fb);
        exit(0);
    }
    poll_input();
}

void DG_SleepMs(uint32_t ms) {
    svcSleepThread((s64)ms * 1000000);
}

uint32_t DG_GetTicksMs(void) {
    return (uint32_t)(armTicksToNs(armGetSystemTick()) / 1000000);
}

int DG_GetKey(int *pressed, unsigned char *key) {
    if (g_queue_head == g_queue_tail) {
        return 0;
    }
    *key = g_queue[g_queue_head].key;
    *pressed = g_queue[g_queue_head].pressed;
    g_queue_head = (g_queue_head + 1) % KEY_QUEUE;
    return 1;
}

void DG_SetWindowTitle(const char *title) {
    say("doom: %s", title);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    Result rc = romfsInit();
    if (R_FAILED(rc)) {
        say("FAIL romfsInit rc=%#x (is --romfs pointing at the directory with doom1.wad?)", rc);
        return 1;
    }
    static char *doom_argv[] = {"doom",   "-iwad", "romfs:/doom1.wad", "-warp", "1",
                                "-skill", "3",     "-nosound",         NULL};
    say("doom: starting E1M1");
    doomgeneric_Create(8, doom_argv);
    while (appletMainLoop()) { /* false on the applet exit request (window closed, HOME) */
        doomgeneric_Tick();
    }
    say("doom: exit requested after %u frames", g_frames);
    framebufferClose(&g_fb);
    return 0;
}

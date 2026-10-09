/*
 * Quake on switch2pc: quakegeneric's platform layer for libnx. The engine
 * renders 320x240 8-bit paletted frames; we expand them through the palette
 * into a libnx framebuffer, feed the pad as keys (right stick as mouse look)
 * and time frames with the system tick.
 *
 * Build: scripts/build_quake.sh (needs third_party/quakegeneric and
 * examples/quake/romfs/id1/pak0.pak, the shareware data).
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "quakegeneric.h"

#ifndef QUAKE_FRAME_LIMIT
#define QUAKE_FRAME_LIMIT 0 /* headless runs stop here; 0 = never */
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
static uint32_t g_palette[256]; /* BGRA words, from QG_SetPalette */
static int g_mouse_x, g_mouse_y;

#define KEY_QUEUE 64
static struct {
    int key;
    int down;
} g_queue[KEY_QUEUE];
static unsigned g_queue_head, g_queue_tail;

static const struct {
    u64 buttons;
    int key;
} k_map[] = {
    {HidNpadButton_A | HidNpadButton_ZR, K_CTRL},  /* attack */
    {HidNpadButton_B | HidNpadButton_ZL, K_SPACE}, /* jump */
    {HidNpadButton_X, '/'},                        /* next weapon */
    {HidNpadButton_Y, K_SHIFT},                    /* run */
    {HidNpadButton_Plus, K_ENTER},
    {HidNpadButton_Minus, K_ESCAPE},
    {HidNpadButton_AnyUp, K_UPARROW},
    {HidNpadButton_AnyDown, K_DOWNARROW},
    {HidNpadButton_AnyLeft, K_LEFTARROW},
    {HidNpadButton_AnyRight, K_RIGHTARROW},
    {HidNpadButton_L, ','},                        /* strafe left */
    {HidNpadButton_R, '.'},                        /* strafe right */
};

static void queue_key(int key, int down) {
    unsigned next = (g_queue_tail + 1) % KEY_QUEUE;
    if (next == g_queue_head) {
        return;
    }
    g_queue[g_queue_tail].key = key;
    g_queue[g_queue_tail].down = down;
    g_queue_tail = next;
}

static void poll_input(void) {
    padUpdate(&g_pad);
    u64 now = padGetButtons(&g_pad);
    if (now != g_prev_buttons) {
        for (size_t i = 0; i < sizeof(k_map) / sizeof(k_map[0]); i++) {
            int was = (g_prev_buttons & k_map[i].buttons) != 0;
            int is = (now & k_map[i].buttons) != 0;
            if (was != is) {
                queue_key(k_map[i].key, is);
            }
        }
        g_prev_buttons = now;
    }
    HidAnalogStickState r = padGetStickPos(&g_pad, 1);
    g_mouse_x += r.x / 2048;
    g_mouse_y -= r.y / 2048;
}

void QG_Init(void) {
    Result rc = framebufferCreate(&g_fb, nwindowGetDefault(), QUAKEGENERIC_RES_X,
                                  QUAKEGENERIC_RES_Y, PIXEL_FORMAT_BGRA_8888, 2);
    if (R_FAILED(rc)) {
        say("FAIL framebufferCreate rc=%#x", rc);
        exit(1);
    }
    framebufferMakeLinear(&g_fb);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    say("quake: framebuffer %ux%u ready", QUAKEGENERIC_RES_X, QUAKEGENERIC_RES_Y);
}

void QG_Quit(void) {
    say("quake: quit after %u frames", g_frames);
    framebufferClose(&g_fb);
}

void QG_SetPalette(unsigned char palette[768]) {
    for (int i = 0; i < 256; i++) {
        uint32_t r = palette[i * 3], g = palette[i * 3 + 1], b = palette[i * 3 + 2];
        g_palette[i] = 0xFF000000u | (r << 16) | (g << 8) | b; /* BGRA in memory */
    }
}

void QG_DrawFrame(void *pixels) {
    const uint8_t *src = pixels;
    u32 stride = 0;
    u8 *fb = framebufferBegin(&g_fb, &stride);
    if (fb) {
        for (unsigned y = 0; y < QUAKEGENERIC_RES_Y; y++) {
            uint32_t *row = (uint32_t *)(fb + (size_t)y * stride);
            const uint8_t *line = src + (size_t)y * QUAKEGENERIC_RES_X;
            for (unsigned x = 0; x < QUAKEGENERIC_RES_X; x++) {
                row[x] = g_palette[line[x]];
            }
        }
        framebufferEnd(&g_fb);
    }
    g_frames++;
    if (g_frames % 100 == 0) {
        say("quake: frame %u", g_frames);
    }
    if (QUAKE_FRAME_LIMIT && g_frames >= QUAKE_FRAME_LIMIT) {
        say("quake: done after %u frames", g_frames);
        framebufferClose(&g_fb);
        exit(0);
    }
    poll_input();
}

int QG_GetKey(int *down, int *key) {
    if (g_queue_head == g_queue_tail) {
        return 0;
    }
    *key = g_queue[g_queue_head].key;
    *down = g_queue[g_queue_head].down;
    g_queue_head = (g_queue_head + 1) % KEY_QUEUE;
    return 1;
}

void QG_GetMouseMove(int *x, int *y) {
    *x = g_mouse_x;
    *y = g_mouse_y;
    g_mouse_x = g_mouse_y = 0;
}

void QG_GetJoyAxes(float *axes) {
    memset(axes, 0, sizeof(float) * QUAKEGENERIC_JOY_MAX_AXES);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    Result rc = romfsInit();
    if (R_FAILED(rc)) {
        say("FAIL romfsInit rc=%#x (is --romfs pointing at the directory with id1/pak0.pak?)", rc);
        return 1;
    }
    static char *quake_argv[] = {"quake", "-basedir", "romfs:", "-nosound", NULL};
    say("quake: starting");
    QG_Create(4, quake_argv);
    double old = (double)armTicksToNs(armGetSystemTick()) / 1e9 - 0.1;
    for (;;) {
        double now = (double)armTicksToNs(armGetSystemTick()) / 1e9;
        QG_Tick(now - old);
        old = now;
    }
    return 0;
}

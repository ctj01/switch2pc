/*
 * libnx input: the standard homebrew main loop. padUpdate reads the hid shared
 * memory every frame; the program reports button presses and exits on Plus,
 * exactly like the devkitPro application template. Run with
 * HOS_INPUT_SCRIPT (see shims/hos_input/input_state.h) or a real gamepad via
 * HOS_INPUT_EVDEV.
 *
 * Build: scripts/build_libnx.sh input
 */

#include <stdarg.h>
#include <stdio.h>
#include <switch.h>

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

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    int frame = 0;
    int plus = 0;
    for (; frame < 900; frame++) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);
        if (down) {
            HidAnalogStickState l = padGetStickPos(&pad, 0);
            say("frame %d: kDown %#llx, left stick %d %d, style %#x", frame,
                (unsigned long long)down, l.x, l.y, padGetStyleSet(&pad));
        }
        if (down & HidNpadButton_Plus) {
            plus = 1;
            say("Plus pressed, leaving the main loop");
            break;
        }
        svcSleepThread(16666666);
    }
    if (!plus) {
        say("FAIL no Plus press within %d frames", frame);
    }
    say("libnx input: done");
    return 0;
}

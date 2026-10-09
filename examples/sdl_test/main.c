/*
 * SDL2 on switch2pc: the plain SDL2 API, no platform code. A window with the
 * software renderer, a queued tone on the default audio device, and a game
 * controller; exercises the SDL build from scripts/build_sdl2.sh end to end.
 *
 * Build: scripts/build_sdl_app.sh sdl_test
 */

#include <SDL.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <switch.h>

#ifndef SDL_TEST_FRAMES
#define SDL_TEST_FRAMES 240
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

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        say("FAIL SDL_Init: %s", SDL_GetError());
        return 1;
    }
    say("sdl: video driver %s, %d joystick(s)", SDL_GetCurrentVideoDriver(), SDL_NumJoysticks());

    SDL_Window *win =
        SDL_CreateWindow("sdl_test", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 640, 480, 0);
    if (!win) {
        say("FAIL SDL_CreateWindow: %s", SDL_GetError());
        return 1;
    }
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) {
        say("FAIL SDL_CreateRenderer: %s", SDL_GetError());
        return 1;
    }
    SDL_RendererInfo info;
    SDL_GetRendererInfo(ren, &info);
    say("sdl: renderer %s", info.name);

    /* audio: one second of 440 Hz, queued */
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (dev) {
        say("sdl: audio device %u: %d Hz, %d channels, driver %s", dev, have.freq, have.channels,
            SDL_GetCurrentAudioDriver());
        static Sint16 tone[48000 * 2];
        for (int i = 0; i < 48000; i++) {
            Sint16 v = (Sint16)(sin(2.0 * M_PI * 440.0 * i / 48000.0) * 6000.0);
            tone[i * 2] = v;
            tone[i * 2 + 1] = v;
        }
        SDL_QueueAudio(dev, tone, sizeof(tone));
        SDL_PauseAudioDevice(dev, 0);
    } else {
        say("sdl: no audio device: %s", SDL_GetError());
    }

    SDL_GameController *pad = SDL_NumJoysticks() > 0 ? SDL_GameControllerOpen(0) : NULL;
    say("sdl: controller %s", pad ? SDL_GameControllerName(pad) : "(none)");

    SDL_Texture *tex =
        SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, 64, 64);
    if (tex) {
        Uint32 px[64 * 64];
        for (int y = 0; y < 64; y++) {
            for (int x = 0; x < 64; x++) {
                px[y * 64 + x] = ((x / 8 + y / 8) & 1) ? 0xFFFFFFFFu : 0xFF2020C0u;
            }
        }
        SDL_UpdateTexture(tex, NULL, px, 64 * 4);
    }

    int buttons_seen = 0;
    int quit = 0;
    for (int frame = 0; frame < SDL_TEST_FRAMES && !quit; frame++) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                quit = 1;
            } else if (e.type == SDL_CONTROLLERBUTTONDOWN) {
                buttons_seen++;
                say("sdl: frame %d: controller button %d down", frame, e.cbutton.button);
            } else if (e.type == SDL_JOYBUTTONDOWN) {
                buttons_seen++;
                say("sdl: frame %d: joystick button %d down", frame, e.jbutton.button);
            }
        }
        SDL_SetRenderDrawColor(ren, 16, 16, 48, 255);
        SDL_RenderClear(ren);
        SDL_Rect r = {(frame * 4) % 600, 200 + (int)(100.0 * sin(frame / 20.0)), 40, 40};
        SDL_SetRenderDrawColor(ren, 255, 160, 0, 255);
        SDL_RenderFillRect(ren, &r);
        if (tex) {
            SDL_Rect dst = {300, 50, 128, 128};
            SDL_RenderCopy(ren, tex, NULL, &dst);
        }
        SDL_RenderPresent(ren);
        SDL_Delay(16); /* pace like a game would */
        if (frame % 60 == 0) {
            say("sdl: frame %d, %u bytes of audio queued", frame,
                dev ? SDL_GetQueuedAudioSize(dev) : 0);
        }
    }
    say("sdl: %d button events, quit %d", buttons_seen, quit);
    if (pad) {
        SDL_GameControllerClose(pad);
    }
    if (dev) {
        SDL_CloseAudioDevice(dev);
    }
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    say("sdl_test: done");
    return 0;
}

/*
 * libnx services: audio output and network status through libnx, as a game
 * would use them. Plays one second of a 440 Hz tone into audout and asks
 * nifm whether there is an internet connection.
 *
 * Build: scripts/build_libnx.sh services
 */

#include <malloc.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

#define SAMPLE_RATE 48000
#define CHANNELS    2
#define SECONDS     1

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    Result rc = audoutInitialize();
    say("audoutInitialize rc=%#x rate=%u channels=%u", rc, audoutGetSampleRate(),
        audoutGetChannelCount());
    if (R_SUCCEEDED(rc)) {
        size_t samples = SAMPLE_RATE * SECONDS;
        size_t size = (samples * CHANNELS * sizeof(int16_t) + 0xFFF) & ~(size_t)0xFFF;
        int16_t *pcm = memalign(0x1000, size);
        for (size_t i = 0; i < samples; i++) {
            int16_t v = (int16_t)(sin(2.0 * M_PI * 440.0 * (double)i / SAMPLE_RATE) * 8000.0);
            pcm[i * 2] = v;
            pcm[i * 2 + 1] = v;
        }
        AudioOutBuffer buffer = {
            .next = NULL,
            .buffer = pcm,
            .buffer_size = size,
            .data_size = samples * CHANNELS * sizeof(int16_t),
            .data_offset = 0,
        };
        rc = audoutStartAudioOut();
        say("audoutStartAudioOut rc=%#x", rc);
        rc = audoutAppendAudioOutBuffer(&buffer);
        say("audoutAppendAudioOutBuffer rc=%#x", rc);
        AudioOutBuffer *released = NULL;
        u32 count = 0;
        rc = audoutWaitPlayFinish(&released, &count, 2000000000ull);
        say("audio released %u buffers rc=%#x (same buffer: %d)", count, rc, released == &buffer);
        u64 played = 0;
        audoutGetAudioOutPlayedSampleCount(&played);
        say("played samples %llu", (unsigned long long)played);
        audoutStopAudioOut();
        audoutExit();
        free(pcm);
    }

    rc = nifmInitialize(NifmServiceType_User);
    say("nifmInitialize rc=%#x", rc);
    if (R_SUCCEEDED(rc)) {
        NifmInternetConnectionType type;
        u32 strength = 0;
        NifmInternetConnectionStatus status;
        rc = nifmGetInternetConnectionStatus(&type, &strength, &status);
        say("internet: %s (rc=%#x)", R_SUCCEEDED(rc) ? "connected" : "offline", rc);
        bool accepted = nifmIsAnyInternetRequestAccepted(nifmGetClientId());
        say("any internet request accepted: %d", accepted);
        nifmExit();
    }

    rc = accountInitialize(AccountServiceType_Application);
    say("accountInitialize rc=%#x", rc);
    if (R_SUCCEEDED(rc)) {
        s32 users = 0;
        AccountUid uids[4];
        s32 listed = 0;
        accountGetUserCount(&users);
        rc = accountListAllUsers(uids, 4, &listed);
        say("account: %d user(s), listed %d rc=%#x", users, listed, rc);
        AccountProfile profile;
        if (listed > 0 && R_SUCCEEDED(accountGetProfile(&profile, uids[0]))) {
            AccountProfileBase base;
            AccountUserData data;
            rc = accountProfileGet(&profile, &data, &base);
            say("account: nickname \"%s\" rc=%#x", base.nickname, rc);
            accountProfileClose(&profile);
        }
        accountExit();
    }

    rc = psmInitialize();
    if (R_SUCCEEDED(rc)) {
        u32 charge = 0;
        PsmChargerType charger = PsmChargerType_Unconnected;
        psmGetBatteryChargePercentage(&charge);
        psmGetChargerType(&charger);
        say("battery: %u%%, charger type %d", charge, (int)charger);
        psmExit();
    } else {
        say("psmInitialize rc=%#x", rc);
    }

    say("libnx services: done");
    return 0;
}

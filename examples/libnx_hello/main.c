/*
 * libnx hello: a real homebrew binary. libnx's own crt0 and __appInit run
 * first (sm, set:sys, applet, hid, fsp-srv, sdmc mount); if main is reached the
 * runtime initialised against our services. Output goes through
 * svcOutputDebugString because there is no console.
 *
 * Build: scripts/build_libnx_hello.sh (needs devkitA64 + libnx).
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <time.h>

static void say(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    svcOutputDebugString(buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
}

static Mutex g_mutex;
static CondVar g_cv;
static int g_counter;
static int g_ready;

static void worker(void *arg) {
    int rounds = *(int *)arg;
    for (int i = 0; i < rounds; i++) {
        mutexLock(&g_mutex);
        g_counter++;
        mutexUnlock(&g_mutex);
        if ((i & 63) == 0) {
            svcSleepThread(0);
        }
    }
    mutexLock(&g_mutex);
    g_ready = 1;
    condvarWakeAll(&g_cv);
    mutexUnlock(&g_mutex);
}

int main(int argc, char **argv) {
    (void)argv;
    say("libnx hello: main reached (argc=%d)", argc);
    u32 ver = hosversionGet();
    say("hos version %u.%u.%u", HOSVER_MAJOR(ver), HOSVER_MINOR(ver), HOSVER_MICRO(ver));

    /* newlib heap on top of svcSetHeapSize */
    size_t big = 4u << 20;
    char *p = malloc(big);
    if (p) {
        memset(p, 0xAB, big);
        say("malloc(%zu) ok, byte %02x", big, (unsigned char)p[big - 1]);
        free(p);
    } else {
        say("FAIL malloc");
    }

    /* time service */
    u64 now = 0;
    Result rc = timeGetCurrentTime(TimeType_UserSystemClock, &now);
    say("timeGetCurrentTime rc=%#x now=%llu", rc, (unsigned long long)now);
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    say("time() = %lld -> %04d-%02d-%02d %02d:%02d UTC", (long long)t, tm.tm_year + 1900,
        tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);

    /* fsp-srv through newlib: sdmc:/ */
    FILE *f = fopen("sdmc:/libnx_hello.txt", "w");
    if (f) {
        fputs("written by libnx\n", f);
        fclose(f);
        char line[64] = {0};
        f = fopen("sdmc:/libnx_hello.txt", "r");
        if (f && fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\n")] = '\0';
            say("sdmc read back: \"%s\"", line);
        } else {
            say("FAIL sdmc read back");
        }
        if (f) {
            fclose(f);
        }
        remove("sdmc:/libnx_hello.txt");
    } else {
        say("sdmc not mounted (errno %d)", errno);
    }

    /* hid shared memory */
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    padUpdate(&pad);
    say("pad buttons %#llx (expect 0: nothing is pressed)",
        (unsigned long long)padGetButtons(&pad));

    /* applet state */
    say("applet focus state %d, operation mode %d", (int)appletGetFocusState(),
        (int)appletGetOperationMode());

    /* libnx threads, mutex and condvar on top of our arbiter */
    mutexInit(&g_mutex);
    condvarInit(&g_cv);
    Thread th;
    int rounds = 5000;
    rc = threadCreate(&th, worker, &rounds, NULL, 0x10000, 0x2C, -2);
    if (R_SUCCEEDED(rc)) {
        rc = threadStart(&th);
    }
    if (R_SUCCEEDED(rc)) {
        mutexLock(&g_mutex);
        while (!g_ready) {
            condvarWait(&g_cv, &g_mutex);
        }
        mutexUnlock(&g_mutex);
        threadWaitForExit(&th);
        threadClose(&th);
        say("thread counter %d (expect %d)", g_counter, rounds);
    } else {
        say("FAIL threadCreate rc=%#x", rc);
    }

    say("libnx hello: done");
    return 0;
}

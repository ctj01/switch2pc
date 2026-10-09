#include <errno.h>
#include <sched.h>
#include <time.h>

#include "cpu_internal.h"

/* svcSleepThread(s64 nanoseconds). Negative values are yield hints. */
void svc_sleep_thread(hos_svc_regs *r) {
    int64_t ns = (int64_t)r->x[0];
    if (ns <= 0) {
        sched_yield();
        return;
    }
    struct timespec ts = {.tv_sec = ns / 1000000000LL, .tv_nsec = ns % 1000000000LL};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

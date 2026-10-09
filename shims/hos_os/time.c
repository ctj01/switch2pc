/*
 * time:u / time:a / time:s. Wall clock from CLOCK_REALTIME, steady clock from
 * CLOCK_MONOTONIC, one UTC time zone.
 *
 * The steady clock is defined so that it matches the 19.2 MHz tick counter
 * libhos_cpu presents: both derive from CLOCK_MONOTONIC, so the runtime can
 * compute "steady base + ticks / 19200000" from the shared memory and get the
 * same seconds the IPC path returns.
 */

#include <string.h>
#include <time.h>

#include "common/hos_log.h"
#include "hos_ipc.h"

#define MOD             "time"
#define TIME_SHMEM_SIZE 0x1000

typedef struct steady_clock_time_point {
    int64_t time_point; /* seconds */
    uint8_t source_id[16];
} steady_clock_time_point;

typedef struct system_clock_context {
    int64_t offset; /* seconds added to the steady clock */
    steady_clock_time_point steady;
} system_clock_context;

typedef struct calendar_time {
    uint16_t year;
    uint8_t month, day, hour, minute, second, pad;
} calendar_time;

typedef struct calendar_additional_info {
    uint32_t day_of_week;
    uint32_t day_of_year;
    char timezone_name[8];
    uint32_t is_dst;
    int32_t utc_offset_seconds;
} calendar_additional_info;

/*
 * Shared memory layout (time sysmodule). Each field is a lock-free double
 * buffer: u32 counter, padding, then two copies; readers take copy[counter & 1].
 *   0x00  steady clock: { s64 base_time; u8 source_id[16] }        (0x38 bytes)
 *   0x38  local system clock context                                (0x48 bytes)
 *   0x80  network system clock context                              (0x48 bytes)
 *   0xC8  user clock automatic correction enabled (bool)
 */
typedef struct shmem_steady {
    uint32_t counter;
    uint32_t pad;
    struct {
        int64_t base_time;
        uint8_t source_id[16];
    } v[2];
} shmem_steady;

typedef struct shmem_context {
    uint32_t counter;
    uint32_t pad;
    system_clock_context v[2];
} shmem_context;

typedef struct shmem_layout {
    shmem_steady steady;
    shmem_context local;
    shmem_context network;
    uint32_t correction_counter;
    uint8_t correction_enabled[2];
} shmem_layout;

_Static_assert(sizeof(shmem_steady) == 0x38, "steady clock block is 0x38 bytes");
_Static_assert(sizeof(shmem_context) == 0x48, "clock context block is 0x48 bytes");
_Static_assert(__builtin_offsetof(shmem_layout, network) == 0x80, "network context at 0x80");
_Static_assert(__builtin_offsetof(shmem_layout, correction_counter) == 0xC8,
               "correction flag at 0xC8");

static const uint8_t k_source_id[16] = {0x53, 0x32, 0x50, 0x43, 0x53, 0x54, 0x45, 0x41,
                                        0x44, 0x59, 0x43, 0x4c, 0x4f, 0x43, 0x4b, 0x30};
static uint32_t g_shmem_handle;
static shmem_layout *g_shmem;

static int64_t now_realtime(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec;
}

static int64_t now_steady(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

static void fill_context(system_clock_context *c) {
    memset(c, 0, sizeof(*c));
    c->steady.time_point = now_steady();
    memcpy(c->steady.source_id, k_source_id, 16);
    c->offset = now_realtime() - c->steady.time_point;
}

static void publish_shmem(void) {
    shmem_layout *s = g_shmem;
    memset(s, 0, sizeof(*s));
    for (int i = 0; i < 2; i++) {
        s->steady.v[i].base_time = 0; /* steady seconds == ticks / 19.2 MHz */
        memcpy(s->steady.v[i].source_id, k_source_id, 16);
        fill_context(&s->local.v[i]);
        fill_context(&s->network.v[i]);
    }
}

/* ---- ISystemClock ------------------------------------------------------------ */

static hos_result system_clock_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case 0: /* GetCurrentTime */ {
        int64_t t = now_realtime();
        hos_ipc_out_write(ctx, &t, 8);
        return HOS_RESULT_SUCCESS;
    }
    case 1: /* SetCurrentTime */
        return HOS_RESULT_SUCCESS;
    case 2: /* GetSystemClockContext */ {
        system_clock_context c;
        fill_context(&c);
        hos_ipc_out_write(ctx, &c, sizeof(c));
        return HOS_RESULT_SUCCESS;
    }
    case 3: /* SetSystemClockContext */
        return HOS_RESULT_SUCCESS;
    case 4: /* GetOperationEventReadableHandle */ {
        void *ev;
        uint32_t h = hos_ipc_event_create(&ev);
        if (!h) {
            return HOS_KERNEL_OUT_OF_RESOURCE;
        }
        ctx->copy_handles[ctx->num_copy++] = h;
        return HOS_RESULT_SUCCESS;
    }
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

static const hos_service_class g_system_clock_class = {
    .name = "time:ISystemClock",
    .dispatch = system_clock_dispatch,
};

/* ---- ISteadyClock -------------------------------------------------------------- */

static hos_result steady_clock_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case 0: /* GetCurrentTimePoint */ {
        steady_clock_time_point tp;
        tp.time_point = now_steady();
        memcpy(tp.source_id, k_source_id, 16);
        hos_ipc_out_write(ctx, &tp, sizeof(tp));
        return HOS_RESULT_SUCCESS;
    }
    case 2:   /* GetTestOffset */
    case 100: /* GetRtcValue */
    case 200: /* GetInternalOffset */ {
        int64_t v = (ctx->cmd == 100) ? now_realtime() : 0;
        hos_ipc_out_write(ctx, &v, 8);
        return HOS_RESULT_SUCCESS;
    }
    case 3: /* SetTestOffset */
        return HOS_RESULT_SUCCESS;
    case 101: /* IsRtcResetDetected */
    case 102: /* GetSetupResultValue */ {
        uint32_t v = 0;
        hos_ipc_out_write(ctx, &v, 4);
        return HOS_RESULT_SUCCESS;
    }
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

static const hos_service_class g_steady_clock_class = {
    .name = "time:ISteadyClock",
    .dispatch = steady_clock_dispatch,
};

/* ---- ITimeZoneService ------------------------------------------------------------ */

static void to_calendar(int64_t posix, calendar_time *ct, calendar_additional_info *ai) {
    time_t t = (time_t)posix;
    struct tm tm;
    gmtime_r(&t, &tm);
    memset(ct, 0, sizeof(*ct));
    ct->year = (uint16_t)(tm.tm_year + 1900);
    ct->month = (uint8_t)(tm.tm_mon + 1);
    ct->day = (uint8_t)tm.tm_mday;
    ct->hour = (uint8_t)tm.tm_hour;
    ct->minute = (uint8_t)tm.tm_min;
    ct->second = (uint8_t)tm.tm_sec;
    memset(ai, 0, sizeof(*ai));
    ai->day_of_week = (uint32_t)tm.tm_wday;
    ai->day_of_year = (uint32_t)tm.tm_yday;
    strcpy(ai->timezone_name, "UTC");
}

static int64_t from_calendar(const calendar_time *ct) {
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = ct->year - 1900;
    tm.tm_mon = ct->month - 1;
    tm.tm_mday = ct->day;
    tm.tm_hour = ct->hour;
    tm.tm_min = ct->minute;
    tm.tm_sec = ct->second;
    return (int64_t)timegm(&tm);
}

static hos_result timezone_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case 0: /* GetDeviceLocationName */ {
        char name[0x24];
        memset(name, 0, sizeof(name));
        strcpy(name, "UTC");
        hos_ipc_out_write(ctx, name, sizeof(name));
        return HOS_RESULT_SUCCESS;
    }
    case 1: /* SetDeviceLocationName */
        return HOS_RESULT_SUCCESS;
    case 2: /* GetTotalLocationNameCount */ {
        uint32_t n = 1;
        hos_ipc_out_write(ctx, &n, 4);
        return HOS_RESULT_SUCCESS;
    }
    case 3: /* LoadLocationNameList(u32 index) -> buffer of 0x24 names, out count */ {
        uint32_t n = 0;
        if (ctx->num_b && ctx->b[0].size >= 0x24) {
            memset((void *)ctx->b[0].addr, 0, 0x24);
            memcpy((void *)ctx->b[0].addr, "UTC", 4);
            n = 1;
        }
        hos_ipc_out_write(ctx, &n, 4);
        return HOS_RESULT_SUCCESS;
    }
    case 4: /* LoadTimeZoneRule(name) -> rule buffer (0x4000) */
        if (ctx->num_b && ctx->b[0].addr) {
            memset((void *)ctx->b[0].addr, 0, ctx->b[0].size);
        }
        return HOS_RESULT_SUCCESS;
    case 5: /* GetTimeZoneRuleVersion */ {
        uint8_t v[16] = {0};
        hos_ipc_out_write(ctx, v, 16);
        return HOS_RESULT_SUCCESS;
    }
    case 100: /* ToCalendarTime(s64 posix, rule) */
    case 101: /* ToCalendarTimeWithMyRule(s64 posix) */ {
        int64_t posix = 0;
        hos_ipc_in_read(ctx, 0, &posix, 8);
        calendar_time ct;
        calendar_additional_info ai;
        to_calendar(posix, &ct, &ai);
        hos_ipc_out_write(ctx, &ct, sizeof(ct));
        hos_ipc_out_write(ctx, &ai, sizeof(ai));
        return HOS_RESULT_SUCCESS;
    }
    case 201: /* ToPosixTime(CalendarTime, rule) -> count, buffer of s64 */
    case 202: /* ToPosixTimeWithMyRule(CalendarTime) */ {
        calendar_time ct;
        if (!hos_ipc_in_read(ctx, 0, &ct, sizeof(ct))) {
            return HOS_KERNEL_INVALID_ARGUMENT;
        }
        int64_t posix = from_calendar(&ct);
        uint32_t n = 0;
        const hos_ipc_buffer *b = ctx->num_b ? &ctx->b[0] : (ctx->num_c ? &ctx->c[0] : NULL);
        if (b && b->size >= 8) {
            memcpy((void *)b->addr, &posix, 8);
            n = 1;
        }
        hos_ipc_out_write(ctx, &n, 4);
        return HOS_RESULT_SUCCESS;
    }
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

static const hos_service_class g_timezone_class = {
    .name = "time:ITimeZoneService",
    .dispatch = timezone_dispatch,
};

/* ---- time:u ----------------------------------------------------------------------- */

static hos_result time_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case 0: /* GetStandardUserSystemClock */
    case 1: /* GetStandardNetworkSystemClock */
    case 4: /* GetStandardLocalSystemClock */
        return hos_ipc_return_object(ctx, hos_ipc_object_new(&g_system_clock_class, NULL));
    case 2: /* GetStandardSteadyClock */
        return hos_ipc_return_object(ctx, hos_ipc_object_new(&g_steady_clock_class, NULL));
    case 3: /* GetTimeZoneService */
        return hos_ipc_return_object(ctx, hos_ipc_object_new(&g_timezone_class, NULL));
    case 20: /* GetSharedMemoryNativeHandle */ {
        if (!g_shmem_handle) {
            void *view = NULL;
            g_shmem_handle = hos_shmem_create(TIME_SHMEM_SIZE, &view);
            if (!g_shmem_handle) {
                return HOS_KERNEL_OUT_OF_RESOURCE;
            }
            g_shmem = view;
            publish_shmem();
        }
        uint32_t h = hos_ipc_copy_handle(g_shmem_handle);
        if (!h) {
            return HOS_KERNEL_OUT_OF_HANDLES;
        }
        ctx->copy_handles[ctx->num_copy++] = h;
        return HOS_RESULT_SUCCESS;
    }
    case 100: /* IsStandardUserSystemClockAutomaticCorrectionEnabled */ {
        uint8_t v = 0;
        hos_ipc_out_write(ctx, &v, 1);
        return HOS_RESULT_SUCCESS;
    }
    case 101: /* SetStandardUserSystemClockAutomaticCorrectionEnabled */
        return HOS_RESULT_SUCCESS;
    case 200: /* IsStandardNetworkSystemClockAccuracySufficient */ {
        uint8_t v = 1;
        hos_ipc_out_write(ctx, &v, 1);
        return HOS_RESULT_SUCCESS;
    }
    case 300: /* CalculateMonotonicSystemClockBaseTimePoint(context) -> s64 */ {
        int64_t v = now_steady();
        hos_ipc_out_write(ctx, &v, 8);
        return HOS_RESULT_SUCCESS;
    }
    case 400: /* GetClockSnapshot(u8 type) -> 0xD0 snapshot in buffer */
    case 401: /* GetClockSnapshotFromSystemClockContext */ {
        const hos_ipc_buffer *b = ctx->num_b ? &ctx->b[0] : (ctx->num_c ? &ctx->c[0] : NULL);
        if (!b || b->size < 0xD0) {
            return HOS_KERNEL_INVALID_SIZE;
        }
        uint8_t *s = (uint8_t *)b->addr;
        memset(s, 0, 0xD0);
        system_clock_context c;
        fill_context(&c);
        memcpy(s + 0x00, &c, sizeof(c)); /* user context */
        memcpy(s + 0x20, &c, sizeof(c)); /* network context */
        int64_t t = now_realtime();
        memcpy(s + 0x40, &t, 8); /* user time */
        memcpy(s + 0x48, &t, 8); /* network time */
        calendar_time ct;
        calendar_additional_info ai;
        to_calendar(t, &ct, &ai);
        memcpy(s + 0x50, &ct, sizeof(ct));
        memcpy(s + 0x58, &ct, sizeof(ct));
        memcpy(s + 0x60, &ai, sizeof(ai));
        memcpy(s + 0x78, &ai, sizeof(ai));
        memcpy(s + 0x90, &c.steady, sizeof(c.steady));
        memcpy(s + 0xA8, "UTC", 4);
        return HOS_RESULT_SUCCESS;
    }
    case 500: /* CalculateStandardUserSystemClockDifferenceByUser */
    case 501: /* CalculateSpanBetween */ {
        int64_t v = 0;
        hos_ipc_out_write(ctx, &v, 8);
        return HOS_RESULT_SUCCESS;
    }
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

const hos_service_class hos_time_class = {
    .name = "time",
    .dispatch = time_dispatch,
    .destroy = NULL,
};

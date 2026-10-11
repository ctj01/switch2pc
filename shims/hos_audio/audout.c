/*
 * audout:u: audio output. With HOS_AUDIO set (see audio_sink.h) each stream
 * gets a host output and a sink thread that writes the game's buffers to it
 * in order; a buffer is released once the device has taken it, so the game's
 * audio thread runs at the device clock. Without an output the sink is null
 * and a timer pacer plays the device's part: a buffer is "played" for as long
 * as its samples last at the stream's rate, then released. Either way the
 * buffer event is signalled on release, like on hardware. With
 * HOS_AUDIO_DUMP=<file> the PCM data is also appended to that file (raw, host
 * byte order, the format the game chose).
 *
 * Releasing on the spot instead was how this started, and it broke SDL games:
 * SDL's audio thread never blocked, hammered the IPC lock and starved the
 * main thread down to a frame per second.
 */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "audio_sink.h"
#include "common/hos_log.h"
#include "hos_ipc.h"

#define MOD                 "audout"
#define AUDOUT_MAX_PENDING  32
#define AUDOUT_MAX_RELEASED 32
#define AUDOUT_POOL         16

enum { PCM_INT16 = 2 };
enum { STATE_STARTED = 0, STATE_STOPPED = 1 };

#define RELEASE_BY_SINK UINT64_MAX /* release_at_ns of a buffer the sink thread owns */

typedef struct pending_buffer {
    uint64_t tag;
    uint64_t release_at_ns; /* CLOCK_MONOTONIC, or RELEASE_BY_SINK */
    uint64_t samples;
    const uint8_t *pcm; /* game memory; the game leaves it alone until release */
    uint64_t bytes;
    int in_flight; /* the sink thread is writing it */
} pending_buffer;

typedef struct audio_out {
    int used;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t state;
    pending_buffer pending[AUDOUT_MAX_PENDING]; /* in play order */
    uint32_t num_pending;
    uint64_t play_cursor_ns; /* when the last queued buffer finishes */
    uint64_t released[AUDOUT_MAX_RELEASED];
    uint32_t num_released;
    uint64_t played_samples;
    uint32_t volume_bits; /* f32 */
    void *event;
    uint32_t event_handle;
    audio_sink *sink; /* NULL: timer pacing only */
    pthread_t sink_thread;
    pthread_cond_t sink_cond;
    int sink_stop;
} audio_out;

/* AudioOutBuffer as the game passes it (0x28 bytes). */
typedef struct audio_out_buffer {
    uint64_t next;
    uint64_t buffer;
    uint64_t buffer_size;
    uint64_t data_size;
    uint64_t data_offset;
} audio_out_buffer;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond;
static pthread_t g_pacer;
static int g_pacer_started;
static audio_out g_outs[AUDOUT_POOL];
static FILE *g_dump;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static const hos_ipc_buffer *in_buffer(const hos_ipc_ctx *ctx) {
    for (uint32_t i = 0; i < ctx->num_a; i++) {
        if (ctx->a[i].addr) {
            return &ctx->a[i];
        }
    }
    for (uint32_t i = 0; i < ctx->num_x; i++) {
        if (ctx->x[i].addr) {
            return &ctx->x[i];
        }
    }
    return NULL;
}

static const hos_ipc_buffer *out_buffer(const hos_ipc_ctx *ctx) {
    for (uint32_t i = 0; i < ctx->num_b; i++) {
        if (ctx->b[i].addr) {
            return &ctx->b[i];
        }
    }
    for (uint32_t i = 0; i < ctx->num_c; i++) {
        if (ctx->c[i].addr) {
            return &ctx->c[i];
        }
    }
    return NULL;
}

/* g_lock held. Move every pending buffer that has finished playing to the released list. */
static void release_due(audio_out *o, uint64_t now) {
    uint32_t n = 0;
    while (n < o->num_pending && o->pending[n].release_at_ns <= now) {
        if (o->num_released < AUDOUT_MAX_RELEASED) {
            o->released[o->num_released++] = o->pending[n].tag;
        } else {
            HOS_LOGW(MOD, "released queue full, dropping tag %#llx",
                     (unsigned long long)o->pending[n].tag);
        }
        o->played_samples += o->pending[n].samples;
        n++;
    }
    if (n) {
        memmove(o->pending, o->pending + n, (size_t)(o->num_pending - n) * sizeof(o->pending[0]));
        o->num_pending -= n;
        if (o->event) {
            hos_ipc_event_signal(o->event);
        }
    }
}

static void *pacer_main(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_lock);
    for (;;) {
        uint64_t now = now_ns();
        uint64_t next = UINT64_MAX;
        for (int i = 0; i < AUDOUT_POOL; i++) {
            audio_out *o = &g_outs[i];
            if (!o->used) {
                continue;
            }
            release_due(o, now);
            if (o->num_pending && o->pending[0].release_at_ns < next) {
                next = o->pending[0].release_at_ns;
            }
        }
        if (next == UINT64_MAX) {
            pthread_cond_wait(&g_cond, &g_lock);
        } else {
            struct timespec ts = {.tv_sec = (time_t)(next / 1000000000ull),
                                  .tv_nsec = (long)(next % 1000000000ull)};
            pthread_cond_timedwait(&g_cond, &g_lock, &ts);
        }
    }
    return NULL;
}

/* g_lock held. */
static void pacer_start(void) {
    if (g_pacer_started) {
        return;
    }
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&g_cond, &attr);
    pthread_condattr_destroy(&attr);
    if (pthread_create(&g_pacer, NULL, pacer_main, NULL) == 0) {
        pthread_detach(g_pacer);
        g_pacer_started = 1;
    } else {
        HOS_LOGW(MOD, "no pacer thread (%s); buffers release at once", strerror(errno));
    }
}

/* g_lock held. Queue the buffer: it is released when its samples would have been played. */
static void consume(audio_out *o, const audio_out_buffer *b, uint64_t tag) {
    uint64_t samples = 0;
    if (b->buffer && b->data_size && b->data_offset <= b->buffer_size &&
        b->data_size <= b->buffer_size - b->data_offset) {
        const uint8_t *pcm = (const uint8_t *)(b->buffer + b->data_offset);
        if (!g_dump) {
            const char *path = getenv("HOS_AUDIO_DUMP");
            if (path && *path) {
                g_dump = fopen(path, "ab");
            }
        }
        if (g_dump) {
            fwrite(pcm, 1, (size_t)b->data_size, g_dump);
            fflush(g_dump);
        }
        samples = b->data_size / (2u * (o->channels ? o->channels : 2));
    }
    if (o->sink && samples && o->num_pending < AUDOUT_MAX_PENDING) {
        o->pending[o->num_pending++] = (pending_buffer){
            .tag = tag,
            .release_at_ns = RELEASE_BY_SINK,
            .samples = samples,
            .pcm = (const uint8_t *)(b->buffer + b->data_offset),
            .bytes = b->data_size,
        };
        pthread_cond_signal(&o->sink_cond);
        return;
    }
    uint64_t now = now_ns();
    uint64_t start = o->play_cursor_ns > now ? o->play_cursor_ns : now;
    uint64_t duration = o->sample_rate ? samples * 1000000000ull / o->sample_rate : 0;
    uint64_t release_at = start + duration;

    if (!g_pacer_started) {
        pacer_start();
    }
    if (o->num_pending < AUDOUT_MAX_PENDING && g_pacer_started && duration) {
        o->pending[o->num_pending++] =
            (pending_buffer){.tag = tag, .release_at_ns = release_at, .samples = samples};
        o->play_cursor_ns = release_at;
        pthread_cond_signal(&g_cond);
        return;
    }
    /* Empty buffer, no pacer, or the queue is full: release right away. */
    if (o->num_released < AUDOUT_MAX_RELEASED) {
        o->released[o->num_released++] = tag;
    } else {
        HOS_LOGW(MOD, "released queue full, dropping tag %#llx", (unsigned long long)tag);
    }
    o->played_samples += samples;
    if (o->event) {
        hos_ipc_event_signal(o->event);
    }
}

/* Writes queued buffers to the host output in order, releasing each once written. */
static void *sink_main(void *arg) {
    audio_out *o = arg;
    pthread_mutex_lock(&g_lock);
    while (!o->sink_stop) {
        uint32_t i = 0;
        while (i < o->num_pending &&
               (o->pending[i].in_flight || o->pending[i].release_at_ns != RELEASE_BY_SINK)) {
            i++;
        }
        if (i == o->num_pending) {
            pthread_cond_wait(&o->sink_cond, &g_lock);
            continue;
        }
        pending_buffer job = o->pending[i];
        o->pending[i].in_flight = 1;
        pthread_mutex_unlock(&g_lock);
        int rc = audio_sink_write(o->sink, job.pcm, (size_t)job.bytes);
        pthread_mutex_lock(&g_lock);
        if (rc != 0) {
            HOS_LOGW(MOD, "host output failed; falling back to timer pacing");
        }
        /* Flush may have released it meanwhile; positions may have shifted. */
        for (uint32_t k = 0; k < o->num_pending; k++) {
            if (o->pending[k].in_flight && o->pending[k].tag == job.tag) {
                memmove(&o->pending[k], &o->pending[k + 1],
                        (size_t)(o->num_pending - k - 1) * sizeof(o->pending[0]));
                o->num_pending--;
                if (o->num_released < AUDOUT_MAX_RELEASED) {
                    o->released[o->num_released++] = job.tag;
                }
                o->played_samples += job.samples;
                if (o->event) {
                    hos_ipc_event_signal(o->event);
                }
                break;
            }
        }
        if (rc != 0) {
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

/* g_lock held. Stop the sink thread and close the output; buffers it still owned are released. */
static void sink_shutdown(audio_out *o) {
    if (!o->sink) {
        return;
    }
    o->sink_stop = 1;
    pthread_cond_signal(&o->sink_cond);
    pthread_mutex_unlock(&g_lock);
    pthread_join(o->sink_thread, NULL);
    pthread_mutex_lock(&g_lock);
    audio_sink_close(o->sink);
    o->sink = NULL;
    pthread_cond_destroy(&o->sink_cond);
    for (uint32_t i = 0; i < o->num_pending; i++) {
        o->pending[i].release_at_ns = 0; /* the timer pacer releases them now */
        o->pending[i].in_flight = 0;
    }
    pthread_cond_signal(&g_cond);
}

static hos_result audio_out_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    audio_out *o = obj->ctx;
    switch (ctx->cmd) {
    case 0: /* GetAudioOutState -> u32 */
        hos_ipc_out_write(ctx, &o->state, 4);
        return HOS_RESULT_SUCCESS;
    case 1: /* Start */
        o->state = STATE_STARTED;
        return HOS_RESULT_SUCCESS;
    case 2: /* Stop */
        o->state = STATE_STOPPED;
        return HOS_RESULT_SUCCESS;
    case 3: /* AppendAudioOutBuffer(u64 tag, buffer) */
    case 7: /* AppendAudioOutBufferAuto */ {
        uint64_t tag = 0;
        hos_ipc_in_read(ctx, 0, &tag, 8);
        const hos_ipc_buffer *b = in_buffer(ctx);
        audio_out_buffer ab;
        if (!b || !b->addr || b->size < sizeof(ab)) {
            return HOS_KERNEL_INVALID_POINTER;
        }
        memcpy(&ab, (const void *)b->addr, sizeof(ab));
        pthread_mutex_lock(&g_lock);
        consume(o, &ab, tag);
        pthread_mutex_unlock(&g_lock);
        return HOS_RESULT_SUCCESS;
    }
    case 4: /* RegisterBufferEvent -> event */ {
        if (!o->event_handle) {
            o->event_handle = hos_ipc_event_create(&o->event);
            if (!o->event_handle) {
                return HOS_KERNEL_OUT_OF_RESOURCE;
            }
        }
        uint32_t h = hos_ipc_copy_handle(o->event_handle);
        if (!h) {
            return HOS_KERNEL_OUT_OF_HANDLES;
        }
        ctx->copy_handles[ctx->num_copy++] = h;
        return HOS_RESULT_SUCCESS;
    }
    case 5: /* GetReleasedAudioOutBuffers -> buffer of u64 tags, u32 count */
    case 8: /* GetReleasedAudioOutBuffersAuto */ {
        const hos_ipc_buffer *b = out_buffer(ctx);
        uint32_t n = 0;
        pthread_mutex_lock(&g_lock);
        release_due(o, now_ns());
        if (b && b->addr) {
            uint32_t cap = (uint32_t)(b->size / 8);
            n = o->num_released < cap ? o->num_released : cap;
            memcpy((void *)b->addr, o->released, (size_t)n * 8);
            memmove(o->released, o->released + n, (size_t)(o->num_released - n) * 8);
            o->num_released -= n;
        }
        if (o->num_released == 0 && o->event) {
            hos_ipc_event_clear(o->event);
        }
        pthread_mutex_unlock(&g_lock);
        hos_ipc_out_write(ctx, &n, 4);
        return HOS_RESULT_SUCCESS;
    }
    case 6: /* ContainsAudioOutBuffer(u64 tag) -> u8 */ {
        uint64_t tag = 0;
        uint8_t v = 0;
        hos_ipc_in_read(ctx, 0, &tag, 8);
        pthread_mutex_lock(&g_lock);
        for (uint32_t i = 0; i < o->num_pending; i++) {
            if (o->pending[i].tag == tag) {
                v = 1;
                break;
            }
        }
        pthread_mutex_unlock(&g_lock);
        hos_ipc_out_write(ctx, &v, 1);
        return HOS_RESULT_SUCCESS;
    }
    case 9: /* GetAudioOutBufferCount -> u32 */ {
        pthread_mutex_lock(&g_lock);
        uint32_t v = o->num_pending;
        pthread_mutex_unlock(&g_lock);
        hos_ipc_out_write(ctx, &v, 4);
        return HOS_RESULT_SUCCESS;
    }
    case 10: /* GetAudioOutPlayedSampleCount -> u64 */ {
        pthread_mutex_lock(&g_lock);
        uint64_t v = o->played_samples;
        pthread_mutex_unlock(&g_lock);
        hos_ipc_out_write(ctx, &v, 8);
        return HOS_RESULT_SUCCESS;
    }
    case 11: /* FlushAudioOutBuffers -> u8: everything queued is released now */ {
        uint8_t v = 1;
        pthread_mutex_lock(&g_lock);
        release_due(o, UINT64_MAX);
        o->play_cursor_ns = 0;
        pthread_mutex_unlock(&g_lock);
        hos_ipc_out_write(ctx, &v, 1);
        return HOS_RESULT_SUCCESS;
    }
    case 12: /* SetAudioOutVolume(f32) */
        hos_ipc_in_read(ctx, 0, &o->volume_bits, 4);
        return HOS_RESULT_SUCCESS;
    case 13: /* GetAudioOutVolume -> f32 */
        hos_ipc_out_write(ctx, &o->volume_bits, 4);
        return HOS_RESULT_SUCCESS;
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

static void audio_out_destroy(hos_object *obj) {
    audio_out *o = obj->ctx;
    pthread_mutex_lock(&g_lock);
    sink_shutdown(o);
    o->used = 0;
    o->num_pending = 0;
    pthread_mutex_unlock(&g_lock);
}

static const hos_service_class g_audio_out_class = {
    .name = "audout:IAudioOut",
    .dispatch = audio_out_dispatch,
    .destroy = audio_out_destroy,
};

static hos_result audout_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case 0: /* ListAudioOuts -> names (0x100 each), u32 count */
    case 2: /* ListAudioOutsAuto */ {
        const hos_ipc_buffer *b = out_buffer(ctx);
        uint32_t n = 0;
        if (b && b->addr && b->size >= 0x100) {
            memset((void *)b->addr, 0, 0x100);
            memcpy((void *)b->addr, "DeviceOut", 10);
            n = 1;
        }
        hos_ipc_out_write(ctx, &n, 4);
        return HOS_RESULT_SUCCESS;
    }
    case 1: /* OpenAudioOut(u32 sample_rate, u32 channels, u64 aruid, pid, name) */
    case 3: /* OpenAudioOutAuto */ {
        /* AudioOutInParameter: u32 sample_rate; u16 reserved; u16 channel_count. */
        uint32_t rate = 0, word1 = 0;
        hos_ipc_in_read(ctx, 0, &rate, 4);
        hos_ipc_in_read(ctx, 4, &word1, 4);
        uint32_t channels = word1 >> 16 ? word1 >> 16 : (word1 & 0xFFFF);
        if (rate == 0) {
            rate = 48000;
        }
        if (channels == 0) {
            channels = 2;
        }
        pthread_mutex_lock(&g_lock);
        audio_out *o = NULL;
        for (int i = 0; i < AUDOUT_POOL; i++) {
            if (!g_outs[i].used) {
                o = &g_outs[i];
                memset(o, 0, sizeof(*o));
                o->used = 1;
                break;
            }
        }
        pthread_mutex_unlock(&g_lock);
        if (!o) {
            return HOS_KERNEL_OUT_OF_RESOURCE;
        }
        o->sample_rate = rate;
        o->channels = channels;
        o->state = STATE_STOPPED;
        o->volume_bits = 0x3F800000; /* 1.0f */
        o->sink = audio_sink_open(rate, channels);
        if (o->sink) {
            pthread_cond_init(&o->sink_cond, NULL);
            o->sink_stop = 0;
            if (pthread_create(&o->sink_thread, NULL, sink_main, o) != 0) {
                audio_sink_close(o->sink);
                o->sink = NULL;
                pthread_cond_destroy(&o->sink_cond);
            }
        }
        const hos_ipc_buffer *name_out = out_buffer(ctx);
        if (name_out && name_out->addr && name_out->size >= 0x100) {
            memset((void *)name_out->addr, 0, 0x100);
            memcpy((void *)name_out->addr, "DeviceOut", 10);
        }
        uint32_t out[4] = {rate, channels, PCM_INT16, STATE_STOPPED};
        hos_ipc_out_write(ctx, out, sizeof(out));
        HOS_LOGD(MOD, "audio out opened: %u Hz, %u channels", rate, channels);
        hos_object *ao = hos_ipc_object_new(&g_audio_out_class, o);
        if (!ao) {
            o->used = 0;
            return HOS_KERNEL_OUT_OF_RESOURCE;
        }
        return hos_ipc_return_object(ctx, ao);
    }
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

const hos_service_class hos_audout_class = {
    .name = "audout:u",
    .dispatch = audout_dispatch,
    .destroy = NULL,
};

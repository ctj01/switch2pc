/*
 * audren:u: the audio renderer, as a sink that plays silence in real time.
 * Games with music and libnx's audio driver (SDL2 on the Switch among them)
 * mix through this service rather than audout: they describe voices, memory
 * pools, mixes and sinks in an update request every 5 ms frame and read back
 * what the renderer did. Here nothing is mixed. The renderer keeps time: a
 * timer signals the system event every frame, memory pools attach and detach
 * when asked, and each voice's wave buffers are consumed at the voice's sample
 * rate on the wall clock, so a game's audio thread paces itself exactly as on
 * the console and its buffers come back to it in order.
 */

#include <inttypes.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#include "common/hos_log.h"
#include "hos_ipc.h"

#define MOD "audren"

#define FRAME_NS      5000000ull /* 200 Hz, AUDREN_TIMER_PERIOD_MS */
#define MAX_RENDERERS 4
#define MAX_VOICES    64
#define MAX_POOLS     (4 * MAX_VOICES + 16)

/* Wire sizes of the update request and response (switchbrew, libnx audren.h). */
#define HDR_SIZE       0x40
#define POOL_IN_SIZE   0x20
#define POOL_OUT_SIZE  0x10
#define VOICE_IN_SIZE  0x170
#define VOICE_OUT_SIZE 0x10
#define WAVEBUF_SIZE   0x38
#define SINK_OUT_SIZE  0x20
#define PERF_OUT_SIZE  0x10
#define BEHAVIOR_OUT   0xB0

enum {
    POOL_INVALID = 0,
    POOL_REQUEST_DETACH = 2,
    POOL_DETACHED = 3,
    POOL_REQUEST_ATTACH = 4,
    POOL_ATTACHED = 5
};
enum { VOICE_STARTED = 0 };
enum { RENDERER_STARTED = 0, RENDERER_STOPPED = 1 };

typedef struct voice_state {
    uint32_t consumed; /* wave buffers consumed so far, cumulative */
    uint64_t played;   /* samples played so far, cumulative */
    uint64_t pos;      /* samples played of the current wave buffer */
    uint64_t last_ns;  /* when the clock last advanced this voice */
    uint64_t budget;   /* samples played by the clock, not yet charged to a buffer */
    int playing;       /* had queued wave buffers at the previous update */
} voice_state;

typedef struct renderer {
    int used;
    uint32_t sample_rate, sample_count, mix_buffer_count, voice_count, sink_count;
    uint32_t state;
    uint32_t time_limit;
    uint32_t event_handle;
    void *event;
    voice_state voices[MAX_VOICES];
} renderer;

static renderer g_renderers[MAX_RENDERERS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_timer;
static int g_timer_running;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* The frame clock: every started renderer's system event, every 5 ms. */
static void *timer_main(void *arg) {
    (void)arg;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    for (;;) {
        next.tv_nsec += FRAME_NS;
        while (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec++;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        pthread_mutex_lock(&g_lock);
        for (int i = 0; i < MAX_RENDERERS; i++) {
            renderer *r = &g_renderers[i];
            if (r->used && r->state == RENDERER_STARTED && r->event) {
                hos_ipc_event_signal(r->event);
            }
        }
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

static const hos_ipc_buffer *nth_buffer(const hos_ipc_buffer *map, uint32_t num_map,
                                        const hos_ipc_buffer *ptr, uint32_t num_ptr, uint32_t n) {
    /* HipcAutoSelect sends both a map-alias and a pointer descriptor; one is empty. */
    if (n < num_map && map[n].addr) {
        return &map[n];
    }
    if (n < num_ptr && ptr[n].addr) {
        return &ptr[n];
    }
    return NULL;
}

static void put_name(const hos_ipc_ctx *ctx, const char *name) {
    const hos_ipc_buffer *b = nth_buffer(ctx->b, ctx->num_b, ctx->c, ctx->num_c, 0);
    if (b && b->addr && b->size) {
        memset((void *)b->addr, 0, b->size);
        strncpy((char *)b->addr, name, b->size - 1);
    }
}

static uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static void wr32(uint8_t *p, uint32_t v) {
    memcpy(p, &v, 4);
}

/*
 * Advance one voice's clock and consume the wave buffer it has played through.
 * Time only passes while the voice has something queued: a buffer handed over
 * after a pause starts playing when it arrives, as on the console. At most one
 * buffer is consumed per update, and never the update that queued it, so the
 * driver always sees a buffer playing before it sees it done: libnx's SDL2
 * audio thread waits for exactly that, and under an emulator one update can
 * cover more than one buffer of time.
 */
static void voice_advance(voice_state *v, const uint8_t *in, uint64_t now) {
    uint32_t is_new = in[8];
    uint32_t state = in[10];
    uint32_t sample_rate = rd32(in + 12);
    uint32_t count = rd32(in + 0x3C);
    uint16_t head;
    memcpy(&head, in + 0x40, 2);
    if (is_new) {
        memset(v, 0, sizeof(*v));
    }
    if (state != VOICE_STARTED || sample_rate == 0 || count == 0) {
        v->playing = 0;
        v->budget = 0;
        return;
    }
    if (!v->playing) {
        v->playing = 1;
        v->last_ns = now;
        v->budget = 0;
        return;
    }
    uint64_t elapsed = (now - v->last_ns) * sample_rate / 1000000000ull;
    v->last_ns += elapsed * 1000000000ull / sample_rate;
    v->budget += elapsed;
    if (v->budget > sample_rate) { /* a stall of over a second is not caught up */
        v->budget = sample_rate;
    }
    const uint8_t *wb = in + 0x60 + (head & 3) * WAVEBUF_SIZE;
    int32_t start, end;
    memcpy(&start, wb + 0x10, 4);
    memcpy(&end, wb + 0x14, 4);
    uint64_t samples = end > start ? (uint64_t)(end - start) : 0;
    if (wb[0x18]) { /* is_looping: never consumed, keeps the voice playing */
        v->played += v->budget;
        v->budget = 0;
        return;
    }
    uint64_t left = samples > v->pos ? samples - v->pos : 0;
    if (v->budget < left) {
        v->pos += v->budget;
        v->played += v->budget;
        v->budget = 0;
        return;
    }
    v->budget -= left;
    v->played += left;
    v->pos = 0;
    v->consumed++;
}

static hos_result request_update(renderer *r, hos_ipc_ctx *ctx) {
    const hos_ipc_buffer *out = nth_buffer(ctx->b, ctx->num_b, ctx->c, ctx->num_c, 0);
    const hos_ipc_buffer *in = nth_buffer(ctx->a, ctx->num_a, ctx->x, ctx->num_x, 0);
    if (!out || !in || in->size < HDR_SIZE || out->size < HDR_SIZE) {
        return HOS_KERNEL_INVALID_SIZE;
    }
    const uint8_t *req = (const uint8_t *)in->addr;
    uint8_t *rsp = (uint8_t *)out->addr;
    uint32_t revision = rd32(req + 0);
    uint32_t behavior_sz = rd32(req + 4);
    uint32_t pools_sz = rd32(req + 8);
    uint32_t voices_sz = rd32(req + 12);
    uint32_t channels_sz = rd32(req + 16);
    uint32_t pools = pools_sz / POOL_IN_SIZE;
    uint32_t voices = voices_sz / VOICE_IN_SIZE;
    const uint8_t *in_pools = req + HDR_SIZE + behavior_sz;
    const uint8_t *in_voices = in_pools + pools_sz + channels_sz;
    if ((uint64_t)(in_voices - req) + voices_sz > in->size) {
        return HOS_KERNEL_INVALID_SIZE;
    }
    uint64_t need = HDR_SIZE + (uint64_t)pools * POOL_OUT_SIZE + (uint64_t)voices * VOICE_OUT_SIZE +
                    (uint64_t)r->sink_count * SINK_OUT_SIZE + PERF_OUT_SIZE + BEHAVIOR_OUT;
    if (need > out->size) {
        HOS_LOGW(MOD, "update: response needs %" PRIu64 " bytes, buffer is %" PRIu64, need,
                 out->size);
        return HOS_KERNEL_INVALID_SIZE;
    }
    memset(rsp, 0, need);
    wr32(rsp + 0, revision);
    wr32(rsp + 4, BEHAVIOR_OUT);
    wr32(rsp + 8, pools * POOL_OUT_SIZE);
    wr32(rsp + 12, voices * VOICE_OUT_SIZE);
    wr32(rsp + 28, r->sink_count * SINK_OUT_SIZE);
    wr32(rsp + 32, PERF_OUT_SIZE);
    wr32(rsp + 60, (uint32_t)need);

    uint8_t *out_pools = rsp + HDR_SIZE;
    for (uint32_t i = 0; i < pools; i++) {
        uint32_t state = rd32(in_pools + i * POOL_IN_SIZE + 16);
        uint32_t new_state = state == POOL_REQUEST_ATTACH   ? POOL_ATTACHED
                             : state == POOL_REQUEST_DETACH ? POOL_DETACHED
                                                            : POOL_INVALID;
        wr32(out_pools + i * POOL_OUT_SIZE, new_state);
    }
    uint8_t *out_voices = out_pools + pools * POOL_OUT_SIZE;
    uint64_t now = now_ns();
    pthread_mutex_lock(&g_lock);
    for (uint32_t i = 0; i < voices && i < MAX_VOICES; i++) {
        const uint8_t *iv = in_voices + i * VOICE_IN_SIZE;
        voice_state *v = &r->voices[i];
        if (iv[9]) { /* is_used */
            voice_advance(v, iv, now);
        }
        memcpy(out_voices + i * VOICE_OUT_SIZE, &v->played, 8);
        wr32(out_voices + i * VOICE_OUT_SIZE + 8, v->consumed);
    }
    pthread_mutex_unlock(&g_lock);
    return HOS_RESULT_SUCCESS;
}

/* ---- IAudioRenderer ------------------------------------------------------------------ */

static hos_result renderer_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    renderer *r = obj->ctx;
    switch (ctx->cmd) {
    case 0: /* GetSampleRate -> u32 */
    case 1: /* GetSampleCount -> u32 */
    case 2: /* GetMixBufferCount -> u32 */
    case 3: /* GetState -> u32 */
    case 9: /* GetRenderingTimeLimit -> u32 */ {
        uint32_t v = ctx->cmd == 0   ? r->sample_rate
                     : ctx->cmd == 1 ? r->sample_count
                     : ctx->cmd == 2 ? r->mix_buffer_count
                     : ctx->cmd == 3 ? r->state
                                     : r->time_limit;
        hos_ipc_out_write(ctx, &v, 4);
        return HOS_RESULT_SUCCESS;
    }
    case 4:  /* RequestUpdate(out buffer, out perf buffer, in buffer) */
    case 10: /* RequestUpdateAuto */
        return request_update(r, ctx);
    case 5: /* Start */
    case 6: /* Stop */
        pthread_mutex_lock(&g_lock);
        r->state = ctx->cmd == 5 ? RENDERER_STARTED : RENDERER_STOPPED;
        pthread_mutex_unlock(&g_lock);
        return HOS_RESULT_SUCCESS;
    case 7: /* QuerySystemEvent -> event */ {
        if (!r->event_handle) {
            r->event_handle = hos_ipc_event_create(&r->event);
            if (!r->event_handle) {
                return HOS_KERNEL_OUT_OF_RESOURCE;
            }
        }
        uint32_t h = hos_ipc_copy_handle(r->event_handle);
        if (!h) {
            return HOS_KERNEL_OUT_OF_HANDLES;
        }
        ctx->copy_handles[ctx->num_copy++] = h;
        return HOS_RESULT_SUCCESS;
    }
    case 8: /* SetRenderingTimeLimit(u32 percent) */
        hos_ipc_in_read(ctx, 0, &r->time_limit, 4);
        return HOS_RESULT_SUCCESS;
    case 11: /* ExecuteAudioRendererRendering */
        return HOS_RESULT_SUCCESS;
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

static void renderer_destroy(hos_object *obj) {
    renderer *r = obj->ctx;
    pthread_mutex_lock(&g_lock);
    r->used = 0;
    r->state = RENDERER_STOPPED;
    pthread_mutex_unlock(&g_lock);
}

static const hos_service_class g_renderer_class = {
    .name = "audren:IAudioRenderer",
    .dispatch = renderer_dispatch,
    .destroy = renderer_destroy,
};

/* ---- IAudioDevice ---------------------------------------------------------------------- */

static hos_result device_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case 0: /* ListAudioDeviceName(out buffer) -> s32 count */
    case 6: /* ListAudioDeviceNameAuto */
    case 14: /* ListAudioOutputDeviceName */ {
        put_name(ctx, "MainAudioOut");
        int32_t n = 1;
        hos_ipc_out_write(ctx, &n, 4);
        return HOS_RESULT_SUCCESS;
    }
    case 1: /* SetAudioDeviceOutputVolume(f32, name) */
    case 7: /* SetAudioDeviceOutputVolumeAuto */
        return HOS_RESULT_SUCCESS;
    case 2: /* GetAudioDeviceOutputVolume(name) -> f32 */
    case 8: /* GetAudioDeviceOutputVolumeAuto */ {
        uint32_t one = 0x3F800000; /* 1.0f */
        hos_ipc_out_write(ctx, &one, 4);
        return HOS_RESULT_SUCCESS;
    }
    case 3:  /* GetActiveAudioDeviceName(out buffer) */
    case 10: /* GetActiveAudioDeviceNameAuto */
    case 13: /* GetActiveAudioOutputDeviceName */
        put_name(ctx, "MainAudioOut");
        return HOS_RESULT_SUCCESS;
    case 4:  /* QueryAudioDeviceSystemEvent -> event */
    case 11: /* QueryAudioDeviceInputEvent */
    case 12: /* QueryAudioDeviceOutputEvent */ {
        /* Never signalled: the output device never changes. */
        void *ev = NULL;
        uint32_t h = hos_ipc_event_create(&ev);
        if (!h) {
            return HOS_KERNEL_OUT_OF_RESOURCE;
        }
        uint32_t copy = hos_ipc_copy_handle(h);
        if (!copy) {
            return HOS_KERNEL_OUT_OF_HANDLES;
        }
        ctx->copy_handles[ctx->num_copy++] = copy;
        return HOS_RESULT_SUCCESS;
    }
    case 5: /* GetActiveChannelCount -> u32 */ {
        uint32_t n = 2;
        hos_ipc_out_write(ctx, &n, 4);
        return HOS_RESULT_SUCCESS;
    }
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

static const hos_service_class g_device_class = {
    .name = "audren:IAudioDevice",
    .dispatch = device_dispatch,
};

/* ---- IAudioRendererManager ------------------------------------------------------------- */

static hos_result open_renderer(hos_ipc_ctx *ctx) {
    /* AudioRendererParameter: s32 sample_rate, sample_count, mix_buffer_count,
     * submix_count, voice_count, sink_count, effect_count, ... */
    uint32_t p[7] = {0};
    hos_ipc_in_read(ctx, 0, p, sizeof(p));
    pthread_mutex_lock(&g_lock);
    renderer *r = NULL;
    for (int i = 0; i < MAX_RENDERERS; i++) {
        if (!g_renderers[i].used) {
            r = &g_renderers[i];
            memset(r, 0, sizeof(*r));
            r->used = 1;
            break;
        }
    }
    if (r && !g_timer_running) {
        if (pthread_create(&g_timer, NULL, timer_main, NULL) == 0) {
            pthread_detach(g_timer);
            g_timer_running = 1;
        } else {
            HOS_LOGE(MOD, "cannot start the frame timer");
        }
    }
    pthread_mutex_unlock(&g_lock);
    if (!r) {
        return HOS_KERNEL_OUT_OF_RESOURCE;
    }
    r->sample_rate = p[0] ? p[0] : 48000;
    r->sample_count = p[1] ? p[1] : 240;
    r->mix_buffer_count = p[2];
    r->voice_count = p[4] > MAX_VOICES ? MAX_VOICES : p[4];
    r->sink_count = p[5];
    r->state = RENDERER_STOPPED;
    r->time_limit = 100;
    HOS_LOGD(MOD, "renderer opened: %u Hz, %u samples per frame, %u voices, %u sinks",
             r->sample_rate, r->sample_count, p[4], p[5]);
    hos_object *o = hos_ipc_object_new(&g_renderer_class, r);
    if (!o) {
        r->used = 0;
        return HOS_KERNEL_OUT_OF_RESOURCE;
    }
    return hos_ipc_return_object(ctx, o);
}

static hos_result manager_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case 0: /* OpenAudioRenderer(params, u64 work size, u64 aruid, pid, handles) */
    case 3: /* OpenAudioRendererForManualExecution */
        return open_renderer(ctx);
    case 1: /* GetWorkBufferSize(params) -> u64 */ {
        /* The console's figure depends on everything in the parameters; the
         * driver only needs a size to allocate, and nothing here uses it. */
        uint32_t p[7] = {0};
        hos_ipc_in_read(ctx, 0, p, sizeof(p));
        uint64_t size = 0x10000 + (uint64_t)p[4] * 0x2000 + (uint64_t)p[2] * p[1] * 4 * 2 +
                        (uint64_t)p[6] * 0x4000;
        size = (size + 0xFFF) & ~0xFFFull;
        hos_ipc_out_write(ctx, &size, 8);
        return HOS_RESULT_SUCCESS;
    }
    case 2: /* GetAudioDeviceService(u64 aruid) -> IAudioDevice */
    case 4: /* GetAudioDeviceServiceWithRevisionInfo(u64 aruid, u32 revision) */
        return hos_ipc_return_object(ctx, hos_ipc_object_new(&g_device_class, NULL));
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

const hos_service_class hos_audren_class = {
    .name = "audren:u",
    .dispatch = manager_dispatch,
    .destroy = NULL,
};

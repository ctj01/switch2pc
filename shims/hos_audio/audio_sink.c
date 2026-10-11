/*
 * Host audio output for audout; see audio_sink.h. The few PulseAudio "simple"
 * and ALSA entry points used here are declared locally with their stable C
 * ABI, so no development headers are needed to build.
 */

#include "audio_sink.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "common/hos_log.h"

#define MOD "audio"

/* Target latency: long enough for qemu's jitter, short enough to feel live. */
#define SINK_LATENCY_US 80000u

/* ---- PulseAudio simple API ----------------------------------------------------- */

typedef struct pa_sample_spec {
    int format; /* pa_sample_format_t */
    uint32_t rate;
    uint8_t channels;
} pa_sample_spec;

typedef struct pa_buffer_attr {
    uint32_t maxlength, tlength, prebuf, minreq, fragsize;
} pa_buffer_attr;

enum { PA_SAMPLE_S16LE = 3, PA_STREAM_PLAYBACK = 1 };

typedef void *(*pa_simple_new_fn)(const char *server, const char *name, int dir, const char *dev,
                                  const char *stream_name, const pa_sample_spec *ss,
                                  const void *map, const pa_buffer_attr *attr, int *error);
typedef int (*pa_simple_write_fn)(void *s, const void *data, size_t bytes, int *error);
typedef int (*pa_simple_drain_fn)(void *s, int *error);
typedef void (*pa_simple_free_fn)(void *s);
typedef const char *(*pa_strerror_fn)(int error);

/* ---- ALSA ---------------------------------------------------------------------- */

enum {
    SND_PCM_STREAM_PLAYBACK = 0,
    SND_PCM_FORMAT_S16_LE = 2,
    SND_PCM_ACCESS_RW_INTERLEAVED = 3,
};

typedef int (*snd_pcm_open_fn)(void **pcm, const char *name, int stream, int mode);
typedef int (*snd_pcm_set_params_fn)(void *pcm, int format, int access, unsigned channels,
                                     unsigned rate, int soft_resample, unsigned latency_us);
typedef long (*snd_pcm_writei_fn)(void *pcm, const void *buf, unsigned long frames);
typedef int (*snd_pcm_recover_fn)(void *pcm, int err, int silent);
typedef int (*snd_pcm_drain_fn)(void *pcm);
typedef int (*snd_pcm_close_fn)(void *pcm);
typedef const char *(*snd_strerror_fn)(int err);

struct audio_sink {
    int kind; /* 1 pulse, 2 alsa */
    void *handle;
    uint32_t frame_bytes;
    struct {
        pa_simple_write_fn write;
        pa_simple_drain_fn drain;
        pa_simple_free_fn free;
    } pa;
    struct {
        snd_pcm_writei_fn writei;
        snd_pcm_recover_fn recover;
        snd_pcm_drain_fn drain;
        snd_pcm_close_fn close;
    } alsa;
};

static void *load(const char *const *names) {
    for (; *names; names++) {
        void *lib = dlopen(*names, RTLD_NOW | RTLD_LOCAL);
        if (lib) {
            return lib;
        }
    }
    return NULL;
}

static audio_sink *open_pulse(uint32_t rate, uint32_t channels) {
    static const char *const names[] = {"libpulse-simple.so.0", "libpulse-simple.so", NULL};
    void *lib = load(names);
    if (!lib) {
        HOS_LOGD(MOD, "no libpulse-simple: %s", dlerror());
        return NULL;
    }
    pa_simple_new_fn new_ = (pa_simple_new_fn)dlsym(lib, "pa_simple_new");
    pa_strerror_fn strerror_ = (pa_strerror_fn)dlsym(lib, "pa_strerror");
    audio_sink s = {.kind = 1, .frame_bytes = 2 * channels};
    s.pa.write = (pa_simple_write_fn)dlsym(lib, "pa_simple_write");
    s.pa.drain = (pa_simple_drain_fn)dlsym(lib, "pa_simple_drain");
    s.pa.free = (pa_simple_free_fn)dlsym(lib, "pa_simple_free");
    if (!new_ || !s.pa.write || !s.pa.drain || !s.pa.free) {
        HOS_LOGW(MOD, "libpulse-simple is missing symbols");
        return NULL;
    }
    pa_sample_spec ss = {.format = PA_SAMPLE_S16LE, .rate = rate, .channels = (uint8_t)channels};
    uint32_t target = (uint32_t)((uint64_t)rate * SINK_LATENCY_US / 1000000u) * s.frame_bytes;
    pa_buffer_attr attr = {.maxlength = (uint32_t)-1,
                           .tlength = target,
                           .prebuf = (uint32_t)-1,
                           .minreq = (uint32_t)-1,
                           .fragsize = (uint32_t)-1};
    int err = 0;
    s.handle = new_(NULL, "switch2pc", PA_STREAM_PLAYBACK, NULL, "game", &ss, NULL, &attr, &err);
    if (!s.handle) {
        HOS_LOGW(MOD, "PulseAudio: %s", strerror_ ? strerror_(err) : "connection failed");
        return NULL;
    }
    audio_sink *out = malloc(sizeof(*out));
    if (out) {
        *out = s;
    } else {
        s.pa.free(s.handle);
    }
    return out;
}

static audio_sink *open_alsa(uint32_t rate, uint32_t channels) {
    static const char *const names[] = {"libasound.so.2", "libasound.so", NULL};
    void *lib = load(names);
    if (!lib) {
        HOS_LOGD(MOD, "no libasound: %s", dlerror());
        return NULL;
    }
    snd_pcm_open_fn open_ = (snd_pcm_open_fn)dlsym(lib, "snd_pcm_open");
    snd_pcm_set_params_fn set_params = (snd_pcm_set_params_fn)dlsym(lib, "snd_pcm_set_params");
    snd_strerror_fn strerror_ = (snd_strerror_fn)dlsym(lib, "snd_strerror");
    audio_sink s = {.kind = 2, .frame_bytes = 2 * channels};
    s.alsa.writei = (snd_pcm_writei_fn)dlsym(lib, "snd_pcm_writei");
    s.alsa.recover = (snd_pcm_recover_fn)dlsym(lib, "snd_pcm_recover");
    s.alsa.drain = (snd_pcm_drain_fn)dlsym(lib, "snd_pcm_drain");
    s.alsa.close = (snd_pcm_close_fn)dlsym(lib, "snd_pcm_close");
    if (!open_ || !set_params || !s.alsa.writei || !s.alsa.recover || !s.alsa.drain ||
        !s.alsa.close) {
        HOS_LOGW(MOD, "libasound is missing symbols");
        return NULL;
    }
    int err = open_(&s.handle, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        HOS_LOGW(MOD, "ALSA open: %s", strerror_ ? strerror_(err) : "failed");
        return NULL;
    }
    err = set_params(s.handle, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, channels, rate,
                     1, SINK_LATENCY_US);
    if (err < 0) {
        HOS_LOGW(MOD, "ALSA params: %s", strerror_ ? strerror_(err) : "failed");
        s.alsa.close(s.handle);
        return NULL;
    }
    audio_sink *out = malloc(sizeof(*out));
    if (out) {
        *out = s;
    } else {
        s.alsa.close(s.handle);
    }
    return out;
}

audio_sink *audio_sink_open(uint32_t rate, uint32_t channels) {
    const char *mode = getenv("HOS_AUDIO");
    if (!mode || !*mode || strcmp(mode, "0") == 0 || strcmp(mode, "null") == 0) {
        return NULL;
    }
    if (rate == 0 || channels == 0 || channels > 8) {
        return NULL;
    }
    int auto_ = strcmp(mode, "auto") == 0 || strcmp(mode, "1") == 0;
    audio_sink *s = NULL;
    if (auto_ || strcmp(mode, "pulse") == 0) {
        s = open_pulse(rate, channels);
    }
    if (!s && (auto_ || strcmp(mode, "alsa") == 0)) {
        s = open_alsa(rate, channels);
    }
    if (s) {
        HOS_LOGI(MOD, "output: %s, %u Hz, %u channels", audio_sink_name(s), rate, channels);
    } else {
        HOS_LOGW(MOD, "HOS_AUDIO=%s but no output could be opened; audio is paced, not played",
                 mode);
    }
    return s;
}

int audio_sink_write(audio_sink *s, const void *pcm, size_t bytes) {
    if (!s || !bytes) {
        return 0;
    }
    if (s->kind == 1) {
        int err = 0;
        return s->pa.write(s->handle, pcm, bytes, &err) < 0 ? -1 : 0;
    }
    const uint8_t *p = pcm;
    unsigned long frames = bytes / s->frame_bytes;
    while (frames > 0) {
        long n = s->alsa.writei(s->handle, p, frames);
        if (n < 0) {
            if (s->alsa.recover(s->handle, (int)n, 1) < 0) {
                return -1;
            }
            continue; /* underrun recovered: retry the same frames */
        }
        p += (size_t)n * s->frame_bytes;
        frames -= (unsigned long)n;
    }
    return 0;
}

void audio_sink_close(audio_sink *s) {
    if (!s) {
        return;
    }
    if (s->kind == 1) {
        int err = 0;
        s->pa.drain(s->handle, &err);
        s->pa.free(s->handle);
    } else {
        s->alsa.drain(s->handle);
        s->alsa.close(s->handle);
    }
    free(s);
}

const char *audio_sink_name(const audio_sink *s) {
    return !s ? "none" : s->kind == 1 ? "pulse" : "alsa";
}

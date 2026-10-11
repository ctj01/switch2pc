#ifndef HOS_AUDIO_SINK_H
#define HOS_AUDIO_SINK_H

#include <stddef.h>
#include <stdint.h>

/*
 * Host audio output for audout (#27). Selected by HOS_AUDIO:
 *
 *   unset, "0", "null"  no output (buffers are paced by a timer instead)
 *   "pulse"             PulseAudio (what WSLg provides)
 *   "alsa"              ALSA "default" device
 *   "auto" or "1"       PulseAudio, then ALSA
 *
 * Both libraries are loaded with dlopen at first use, so neither is a build
 * or link dependency. Writes block until the device has room, which is what
 * paces the game's audio thread at the device's own clock.
 */

typedef struct audio_sink audio_sink;

/** Open an interleaved signed 16-bit stream, or NULL when disabled or unavailable. */
audio_sink *audio_sink_open(uint32_t rate, uint32_t channels);

/** Write `bytes` of PCM; blocks while the device buffer is full. Returns 0 on success. */
int audio_sink_write(audio_sink *s, const void *pcm, size_t bytes);

/** Play out what is queued and close. */
void audio_sink_close(audio_sink *s);

/** Name of the backend behind `s` ("pulse" or "alsa"). */
const char *audio_sink_name(const audio_sink *s);

#endif /* HOS_AUDIO_SINK_H */

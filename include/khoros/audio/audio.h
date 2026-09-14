#ifndef KHOROS_AUDIO_AUDIO_H
#define KHOROS_AUDIO_AUDIO_H

#if __STDC_VERSION__ < 202311L
#error "Khoros requires pure ISO C23 (compile with -std=c23)"
#endif

#ifdef _STDBOOL_H
#error "<stdbool.h> is strictly banned. In C23, bool, true, and false are native language keywords."
#endif

#ifdef _STDALIGN_H
#error "<stdalign.h> is strictly banned. In C23, alignas and alignof are native language keywords."
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <pthread.h>
#include "khoros/core/attributes.h"
#include "khoros/audio/alsa.h"
#include "khoros/audio/mixer.h"
#include "khoros/uring/ring.h"

constexpr uint32_t KHR_AUDIO_RING_BUFFER_COUNT = 4;
constexpr uint32_t KHR_AUDIO_CHUNK_FRAMES       = 512;
constexpr size_t   KHR_AUDIO_CHUNK_BYTES        = KHR_AUDIO_CHUNK_FRAMES * 2 * sizeof(int16_t);

typedef enum {
    KHR_AUDIO_BACKEND_AUTO = 0,     /* PipeWire pw-cat or PulseAudio paplay pipe if available, else ALSA */
    KHR_AUDIO_BACKEND_PIPEWIRE,     /* PipeWire pw-cat raw PCM pipe */
    KHR_AUDIO_BACKEND_PULSE,        /* PulseAudio paplay raw PCM pipe */
    KHR_AUDIO_BACKEND_ALSA,         /* Direct kernel /dev/snd/pcmC*D*p */
    KHR_AUDIO_BACKEND_NULL,         /* /dev/null silent sink */
} khr_audio_backend_t;

typedef struct {
    uint32_t            sample_rate;
    uint32_t            period_frames;
    uint32_t            alsa_card;
    uint32_t            alsa_device;
    int                 custom_sink_fd; /* -1 for auto/hardware, >= 0 for custom mock pipe */
    khr_audio_backend_t backend;
    bool                use_io_uring;
    bool                disabled;       /* true to disable audio hardware access (null sink) */
} khr_audio_config_t;

typedef struct {
    khr_alsa_pcm_t      alsa;
    khr_audio_mixer_t   mixer;
    khr_uring_t         ring;
    bool                ring_live;
    bool                buffers_registered;
    int                 timer_fd;
    uint32_t            sample_rate;
    uint32_t            period_frames;
    size_t              chunk_bytes;
    uint32_t            write_buffer_idx;
    pthread_t           worker;
    _Atomic bool        running;
    _Atomic bool        worker_started;
    pthread_mutex_t     lock;
    pid_t               pipe_child_pid;
    khr_audio_backend_t active_backend;
    char                backend_description[64];
    alignas(64) int16_t chunk_buffers[KHR_AUDIO_RING_BUFFER_COUNT][KHR_AUDIO_CHUNK_FRAMES * 2];
} khr_audio_engine_t;

[[nodiscard]]
bool khr_audio_engine_init(khr_audio_engine_t* engine, const khr_audio_config_t* cfg);

void khr_audio_engine_destroy(khr_audio_engine_t* engine);

[[nodiscard]]
const char* khr_audio_engine_get_backend_name(const khr_audio_engine_t* engine);

/* Process one period/chunk of audio: mixes active voices and submits buffer */
[[nodiscard]]
bool khr_audio_engine_tick(khr_audio_engine_t* engine);

[[nodiscard]]
bool khr_audio_engine_start_worker(khr_audio_engine_t* engine);

void khr_audio_engine_stop_worker(khr_audio_engine_t* engine);

[[nodiscard]]
uint32_t khr_audio_engine_play(khr_audio_engine_t* engine, const khr_audio_clip_t* clip,
                              float x, float y, float z, float gain, bool loop);

void khr_audio_engine_set_listener(khr_audio_engine_t* engine, const khr_audio_listener_t* listener);

#endif /* KHOROS_AUDIO_AUDIO_H */

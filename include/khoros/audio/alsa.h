#ifndef KHOROS_AUDIO_ALSA_H
#define KHOROS_AUDIO_ALSA_H

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
#include <sound/asound.h>
#include "khoros/core/attributes.h"

constexpr uint32_t KHR_AUDIO_DEFAULT_SAMPLE_RATE   = 48'000;
constexpr uint32_t KHR_AUDIO_DEFAULT_CHANNELS      = 2;
constexpr uint32_t KHR_AUDIO_DEFAULT_PERIOD_FRAMES = 512;
constexpr uint32_t KHR_AUDIO_DEFAULT_BUFFER_FRAMES = 2'048;

typedef struct {
    int      fd;
    bool     is_mock;
    bool     is_prepared;
    uint32_t card;
    uint32_t device;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t period_frames;
    uint32_t buffer_frames;
    size_t   frame_bytes;
    size_t   period_bytes;
    size_t   buffer_bytes;
} khr_alsa_pcm_t;

[[nodiscard]]
bool khr_alsa_pcm_open(khr_alsa_pcm_t* pcm, uint32_t card, uint32_t device,
                       uint32_t sample_rate, uint32_t channels, uint32_t period_frames);

[[nodiscard]]
bool khr_alsa_pcm_open_mock(khr_alsa_pcm_t* pcm, int sink_fd,
                            uint32_t sample_rate, uint32_t channels, uint32_t period_frames);

[[nodiscard]]
bool khr_alsa_pcm_prepare(khr_alsa_pcm_t* pcm);

[[nodiscard]]
int64_t khr_alsa_pcm_write(khr_alsa_pcm_t* pcm, const void* frames, uint32_t frame_count);

void khr_alsa_pcm_close(khr_alsa_pcm_t* pcm);

#endif /* KHOROS_AUDIO_ALSA_H */

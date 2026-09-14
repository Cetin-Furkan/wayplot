#ifndef KHOROS_AUDIO_SYNTH_H
#define KHOROS_AUDIO_SYNTH_H

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
#include "khoros/core/attributes.h"
#include "khoros/audio/mixer.h"

/*
 * Procedural Physical Sound Synthesis (Pillar 4).
 * Synthesizes modal harmonics, impact transients, thuds, and clicks
 * without any external audio files or third-party middleware.
 */

constexpr uint32_t KHR_AUDIO_SYNTH_MAX_MODES = 8;

typedef struct {
    float freq_ratio; /* Frequency multiplier relative to base_freq */
    float amplitude;  /* Initial relative mode amplitude */
    float decay_rate; /* Exponential damping factor (1/seconds) */
    float phase;      /* Initial phase offset in radians */
} khr_audio_modal_t;

typedef struct {
    float             base_freq;
    float             duration;    /* In seconds */
    float             attack_time; /* Linear attack in seconds (e.g. 0.002f) */
    float             noise_gain;  /* Transient noise burst amplitude */
    float             noise_decay; /* Noise burst damping factor */
    uint32_t          mode_count;
    khr_audio_modal_t modes[KHR_AUDIO_SYNTH_MAX_MODES];
} khr_audio_synth_def_t;

/*
 * Synthesize raw float audio samples from modal definition into caller buffer.
 * Returns the number of frames generated.
 */
[[nodiscard]]
uint32_t khr_audio_synth_generate(const khr_audio_synth_def_t* def,
                                  uint32_t sample_rate,
                                  float* out_frames,
                                  uint32_t max_frames);

/*
 * High-Level Material Sound Presets:
 * Fills a khr_audio_clip_t structure wrapping caller-provided sample buffer.
 */

/* Metallic / Solid Resonant Impact (Suzanne / Mesh Collision) */
[[nodiscard]]
bool khr_audio_synth_impact(khr_audio_clip_t* clip,
                            float* sample_buf,
                            uint32_t max_frames,
                            uint32_t sample_rate,
                            float base_freq,
                            float duration);

/* Ground / Floor / Heavy Pedestal Thud */
[[nodiscard]]
bool khr_audio_synth_thud(khr_audio_clip_t* clip,
                          float* sample_buf,
                          uint32_t max_frames,
                          uint32_t sample_rate,
                          float base_freq,
                          float duration);

/* Crisp Transient Click / Tap */
[[nodiscard]]
bool khr_audio_synth_click(khr_audio_clip_t* clip,
                           float* sample_buf,
                           uint32_t max_frames,
                           uint32_t sample_rate,
                           float base_freq,
                           float duration);

/* Resonant Ambient Hum */
[[nodiscard]]
bool khr_audio_synth_hum(khr_audio_clip_t* clip,
                         float* sample_buf,
                         uint32_t max_frames,
                         uint32_t sample_rate,
                         float base_freq,
                         float duration);

#endif /* KHOROS_AUDIO_SYNTH_H */

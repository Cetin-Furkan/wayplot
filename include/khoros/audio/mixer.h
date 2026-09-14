#ifndef KHOROS_AUDIO_MIXER_H
#define KHOROS_AUDIO_MIXER_H

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

constexpr float    KHR_AUDIO_SPEED_OF_SOUND = 343.3f; /* m/s in air at 20 deg C */
constexpr uint32_t KHR_AUDIO_MAX_VOICES     = 64;
constexpr uint32_t KHR_AUDIO_MIX_CHUNK_CAP  = 1'024;

typedef struct {
    uint32_t     id;
    uint32_t     sample_rate;
    uint32_t     channels;     /* 1 = mono, 2 = interleaved stereo */
    uint32_t     frame_count;
    const float* samples;      /* Normalized [-1.0f, +1.0f] float samples */
    uint64_t     bda_address;  /* Optional Vulkan BDA pointer */
} khr_audio_clip_t;

typedef struct {
    float px, py, pz; /* Position in world space */
    float vx, vy, vz; /* Velocity vector in m/s */
    float fx, fy, fz; /* Normalized forward vector */
    float ux, uy, uz; /* Normalized up vector */
    float rx, ry, rz; /* Normalized right vector (f x u) */
} khr_audio_listener_t;

typedef struct {
    uint32_t                voice_id;
    const khr_audio_clip_t* clip;
    float                   px, py, pz; /* Position in world space */
    float                   vx, vy, vz; /* Velocity in m/s */
    float                   gain;       /* Base linear gain */
    float                   pitch;      /* Base pitch multiplier (1.0 = normal) */
    float                   min_dist;   /* Inner radius with gain 1.0 */
    float                   max_dist;   /* Cutoff distance */
    float                   rolloff;    /* Attenuation rolloff factor */
    double                  playhead;   /* Current frame index in clip (fractional) */
    bool                    loop;
    bool                    active;
    bool                    paused;
    float                   eff_gain_l; /* Evaluated effective gain left */
    float                   eff_gain_r; /* Evaluated effective gain right */
    float                   eff_pitch;  /* Evaluated effective pitch (including Doppler) */
} khr_audio_voice_t;

typedef struct {
    uint32_t             sample_rate;
    khr_audio_listener_t listener;
    khr_audio_voice_t    voices[KHR_AUDIO_MAX_VOICES];
    uint32_t             next_voice_id;
    alignas(64) float    accum_l[KHR_AUDIO_MIX_CHUNK_CAP];
    alignas(64) float    accum_r[KHR_AUDIO_MIX_CHUNK_CAP];
} khr_audio_mixer_t;

void khr_audio_mixer_init(khr_audio_mixer_t* mixer, uint32_t sample_rate);

void khr_audio_mixer_destroy(khr_audio_mixer_t* mixer);

void khr_audio_mixer_set_listener(khr_audio_mixer_t* mixer, const khr_audio_listener_t* listener);

[[nodiscard]]
uint32_t khr_audio_mixer_play(khr_audio_mixer_t* mixer, const khr_audio_clip_t* clip,
                             float x, float y, float z, float gain, bool loop);

void khr_audio_mixer_stop(khr_audio_mixer_t* mixer, uint32_t voice_id);

void khr_audio_mixer_pause(khr_audio_mixer_t* mixer, uint32_t voice_id);

void khr_audio_mixer_resume(khr_audio_mixer_t* mixer, uint32_t voice_id);

void khr_audio_mixer_set_voice_pos(khr_audio_mixer_t* mixer, uint32_t voice_id,
                                  float x, float y, float z);

void khr_audio_mixer_set_voice_vel(khr_audio_mixer_t* mixer, uint32_t voice_id,
                                  float vx, float vy, float vz);

void khr_audio_mixer_set_voice_gain(khr_audio_mixer_t* mixer, uint32_t voice_id, float gain);

void khr_audio_mixer_set_voice_pitch(khr_audio_mixer_t* mixer, uint32_t voice_id, float pitch);

void khr_audio_calc_spatial(const khr_audio_listener_t* listener,
                           const khr_audio_voice_t* voice,
                           float* out_gain_l, float* out_gain_r, float* out_pitch);

/* SIMD-accelerated mixing into interleaved 16-bit stereo PCM chunk */
uint32_t khr_audio_mix_chunk_s16(khr_audio_mixer_t* mixer, int16_t* out_pcm, uint32_t frames);

#endif /* KHOROS_AUDIO_MIXER_H */

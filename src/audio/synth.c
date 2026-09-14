#include "khoros/audio/synth.h"
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

/* Deterministic 32-bit xorshift PRNG for noise transients */
static inline float khr_synth_prng(uint32_t* state) {
    uint32_t x = *state;
    if (x == 0) x = 0x12345678U;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return ((float)(x & 0xFFFFU) / 32767.5f) - 1.0f;
}

[[nodiscard]]
uint32_t khr_audio_synth_generate(const khr_audio_synth_def_t* def,
                                  uint32_t sample_rate,
                                  float* out_frames,
                                  uint32_t max_frames) {
    if (def == nullptr || out_frames == nullptr || sample_rate == 0 || max_frames == 0) {
        return 0;
    }

    uint32_t target_frames = (uint32_t)ceilf(def->duration * (float)sample_rate);
    if (target_frames > max_frames) {
        target_frames = max_frames;
    }
    if (target_frames == 0) {
        return 0;
    }

    uint32_t rng_state = 0xCAFEBABE;
    float peak = 0.0f;

    for (uint32_t i = 0; i < target_frames; i++) {
        float t = (float)i / (float)sample_rate;
        float sample = 0.0f;

        /* Sum modal harmonic oscillators with exponential damping */
        for (uint32_t m = 0; m < def->mode_count && m < KHR_AUDIO_SYNTH_MAX_MODES; m++) {
            float freq = def->base_freq * def->modes[m].freq_ratio;
            float damping = expf(-def->modes[m].decay_rate * t);
            float phase = def->modes[m].phase + (2.0f * (float)M_PI * freq * t);
            sample += def->modes[m].amplitude * sinf(phase) * damping;
        }

        /* Smooth linear attack envelope */
        if (def->attack_time > 1e-6f && t < def->attack_time) {
            sample *= (t / def->attack_time);
        }

        /* Transient physical noise burst */
        if (def->noise_gain > 1e-6f) {
            float noise_damping = expf(-def->noise_decay * t);
            sample += def->noise_gain * khr_synth_prng(&rng_state) * noise_damping;
        }

        out_frames[i] = sample;
        float abs_s = fabsf(sample);
        if (abs_s > peak) {
            peak = abs_s;
        }
    }

    /* Normalize to prevent digital clipping if modes constructively interfere */
    if (peak > 0.95f) {
        float scale = 0.95f / peak;
        for (uint32_t i = 0; i < target_frames; i++) {
            out_frames[i] *= scale;
        }
    }

    return target_frames;
}

[[nodiscard]]
bool khr_audio_synth_impact(khr_audio_clip_t* clip,
                            float* sample_buf,
                            uint32_t max_frames,
                            uint32_t sample_rate,
                            float base_freq,
                            float duration) {
    if (clip == nullptr || sample_buf == nullptr || max_frames == 0 || sample_rate == 0) {
        return false;
    }

    float freq = (base_freq > 20.0f) ? base_freq : 520.0f;
    float dur = (duration > 0.01f) ? duration : 0.45f;

    /* Metallic / solid chime modal distribution (1.0, 2.15, 3.18, 4.35) */
    khr_audio_synth_def_t def = {
        .base_freq = freq,
        .duration = dur,
        .attack_time = 0.001f,
        .noise_gain = 0.0f, /* Zero white-noise parasite: pure harmonic resonance */
        .noise_decay = 80.0f,
        .mode_count = 4,
        .modes = {
            { .freq_ratio = 1.00f, .amplitude = 0.50f, .decay_rate = 8.0f,  .phase = 0.0f },
            { .freq_ratio = 2.15f, .amplitude = 0.35f, .decay_rate = 14.0f, .phase = 0.5f },
            { .freq_ratio = 3.18f, .amplitude = 0.25f, .decay_rate = 22.0f, .phase = 1.0f },
            { .freq_ratio = 4.35f, .amplitude = 0.15f, .decay_rate = 35.0f, .phase = 1.5f },
        },
    };

    uint32_t frames = khr_audio_synth_generate(&def, sample_rate, sample_buf, max_frames);
    if (frames == 0) {
        return false;
    }

    *clip = (khr_audio_clip_t){
        .id = 101,
        .sample_rate = sample_rate,
        .channels = 1,
        .frame_count = frames,
        .samples = sample_buf,
        .bda_address = 0,
    };
    return true;
}

[[nodiscard]]
bool khr_audio_synth_thud(khr_audio_clip_t* clip,
                          float* sample_buf,
                          uint32_t max_frames,
                          uint32_t sample_rate,
                          float base_freq,
                          float duration) {
    if (clip == nullptr || sample_buf == nullptr || max_frames == 0 || sample_rate == 0) {
        return false;
    }

    float freq = (base_freq > 20.0f) ? base_freq : 85.0f;
    float dur = (duration > 0.01f) ? duration : 0.30f;

    /* Low frequency ground thud with rapid dissipation */
    khr_audio_synth_def_t def = {
        .base_freq = freq,
        .duration = dur,
        .attack_time = 0.002f,
        .noise_gain = 0.0f, /* Zero white-noise parasite: clean bass body contact */
        .noise_decay = 65.0f,
        .mode_count = 3,
        .modes = {
            { .freq_ratio = 1.00f, .amplitude = 0.70f, .decay_rate = 28.0f, .phase = 0.0f },
            { .freq_ratio = 1.52f, .amplitude = 0.30f, .decay_rate = 44.0f, .phase = 0.4f },
            { .freq_ratio = 2.40f, .amplitude = 0.15f, .decay_rate = 60.0f, .phase = 0.8f },
        },
    };

    uint32_t frames = khr_audio_synth_generate(&def, sample_rate, sample_buf, max_frames);
    if (frames == 0) {
        return false;
    }

    *clip = (khr_audio_clip_t){
        .id = 102,
        .sample_rate = sample_rate,
        .channels = 1,
        .frame_count = frames,
        .samples = sample_buf,
        .bda_address = 0,
    };
    return true;
}

[[nodiscard]]
bool khr_audio_synth_click(khr_audio_clip_t* clip,
                           float* sample_buf,
                           uint32_t max_frames,
                           uint32_t sample_rate,
                           float base_freq,
                           float duration) {
    if (clip == nullptr || sample_buf == nullptr || max_frames == 0 || sample_rate == 0) {
        return false;
    }

    float freq = (base_freq > 20.0f) ? base_freq : 1800.0f;
    float dur = (duration > 0.005f) ? duration : 0.05f;

    /* Crisp transient tap */
    khr_audio_synth_def_t def = {
        .base_freq = freq,
        .duration = dur,
        .attack_time = 0.0005f,
        .noise_gain = 0.0f, /* Clean transient strike */
        .noise_decay = 140.0f,
        .mode_count = 2,
        .modes = {
            { .freq_ratio = 1.00f, .amplitude = 0.60f, .decay_rate = 95.0f,  .phase = 0.0f },
            { .freq_ratio = 2.25f, .amplitude = 0.40f, .decay_rate = 130.0f, .phase = 0.2f },
        },
    };

    uint32_t frames = khr_audio_synth_generate(&def, sample_rate, sample_buf, max_frames);
    if (frames == 0) {
        return false;
    }

    *clip = (khr_audio_clip_t){
        .id = 103,
        .sample_rate = sample_rate,
        .channels = 1,
        .frame_count = frames,
        .samples = sample_buf,
        .bda_address = 0,
    };
    return true;
}

[[nodiscard]]
bool khr_audio_synth_hum(khr_audio_clip_t* clip,
                         float* sample_buf,
                         uint32_t max_frames,
                         uint32_t sample_rate,
                         float base_freq,
                         float duration) {
    if (clip == nullptr || sample_buf == nullptr || max_frames == 0 || sample_rate == 0) {
        return false;
    }

    float freq = (base_freq > 20.0f) ? base_freq : 120.0f;
    float dur = (duration > 0.01f) ? duration : 1.0f;

    /* Ambient harmonic hum */
    khr_audio_synth_def_t def = {
        .base_freq = freq,
        .duration = dur,
        .attack_time = 0.05f,
        .noise_gain = 0.0f,
        .noise_decay = 0.0f,
        .mode_count = 3,
        .modes = {
            { .freq_ratio = 1.00f, .amplitude = 0.60f, .decay_rate = 0.2f, .phase = 0.0f },
            { .freq_ratio = 2.00f, .amplitude = 0.30f, .decay_rate = 0.4f, .phase = 0.5f },
            { .freq_ratio = 3.00f, .amplitude = 0.15f, .decay_rate = 0.8f, .phase = 1.0f },
        },
    };

    uint32_t frames = khr_audio_synth_generate(&def, sample_rate, sample_buf, max_frames);
    if (frames == 0) {
        return false;
    }

    *clip = (khr_audio_clip_t){
        .id = 104,
        .sample_rate = sample_rate,
        .channels = 1,
        .frame_count = frames,
        .samples = sample_buf,
        .bda_address = 0,
    };
    return true;
}

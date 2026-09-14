#include "khoros/audio/mixer.h"

#include <math.h>
#include <string.h>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

void khr_audio_mixer_init(khr_audio_mixer_t* mixer, uint32_t sample_rate) {
    if (mixer == nullptr) {
        return;
    }
    memset(mixer, 0, sizeof(*mixer));
    mixer->sample_rate = (sample_rate > 0) ? sample_rate : 48'000;
    mixer->next_voice_id = 1;

    /* Default listener looking down -Z, Y-up, right +X */
    mixer->listener.px = 0.0f;
    mixer->listener.py = 0.0f;
    mixer->listener.pz = 0.0f;
    mixer->listener.vx = 0.0f;
    mixer->listener.vy = 0.0f;
    mixer->listener.vz = 0.0f;
    mixer->listener.fx = 0.0f;
    mixer->listener.fy = 0.0f;
    mixer->listener.fz = -1.0f;
    mixer->listener.ux = 0.0f;
    mixer->listener.uy = 1.0f;
    mixer->listener.uz = 0.0f;
    mixer->listener.rx = 1.0f;
    mixer->listener.ry = 0.0f;
    mixer->listener.rz = 0.0f;
}

void khr_audio_mixer_destroy(khr_audio_mixer_t* mixer) {
    if (mixer == nullptr) {
        return;
    }
    memset(mixer, 0, sizeof(*mixer));
}

void khr_audio_mixer_set_listener(khr_audio_mixer_t* mixer, const khr_audio_listener_t* listener) {
    if (mixer == nullptr || listener == nullptr) {
        return;
    }
    mixer->listener = *listener;

    /* Normalize forward vector */
    float flen = sqrtf(mixer->listener.fx * mixer->listener.fx +
                       mixer->listener.fy * mixer->listener.fy +
                       mixer->listener.fz * mixer->listener.fz);
    if (flen > 1e-6f) {
        mixer->listener.fx /= flen;
        mixer->listener.fy /= flen;
        mixer->listener.fz /= flen;
    } else {
        mixer->listener.fx = 0.0f;
        mixer->listener.fy = 0.0f;
        mixer->listener.fz = -1.0f;
    }

    /* Normalize up vector */
    float ulen = sqrtf(mixer->listener.ux * mixer->listener.ux +
                       mixer->listener.uy * mixer->listener.uy +
                       mixer->listener.uz * mixer->listener.uz);
    if (ulen > 1e-6f) {
        mixer->listener.ux /= ulen;
        mixer->listener.uy /= ulen;
        mixer->listener.uz /= ulen;
    } else {
        mixer->listener.ux = 0.0f;
        mixer->listener.uy = 1.0f;
        mixer->listener.uz = 0.0f;
    }

    /* Compute right = forward x up */
    mixer->listener.rx = mixer->listener.fy * mixer->listener.uz - mixer->listener.fz * mixer->listener.uy;
    mixer->listener.ry = mixer->listener.fz * mixer->listener.ux - mixer->listener.fx * mixer->listener.uz;
    mixer->listener.rz = mixer->listener.fx * mixer->listener.uy - mixer->listener.fy * mixer->listener.ux;

    float rlen = sqrtf(mixer->listener.rx * mixer->listener.rx +
                       mixer->listener.ry * mixer->listener.ry +
                       mixer->listener.rz * mixer->listener.rz);
    if (rlen > 1e-6f) {
        mixer->listener.rx /= rlen;
        mixer->listener.ry /= rlen;
        mixer->listener.rz /= rlen;
    } else {
        mixer->listener.rx = 1.0f;
        mixer->listener.ry = 0.0f;
        mixer->listener.rz = 0.0f;
    }
}

void khr_audio_calc_spatial(const khr_audio_listener_t* listener,
                           const khr_audio_voice_t* voice,
                           float* out_gain_l, float* out_gain_r, float* out_pitch) {
    if (listener == nullptr || voice == nullptr) {
        if (out_gain_l) *out_gain_l = 0.0f;
        if (out_gain_r) *out_gain_r = 0.0f;
        if (out_pitch)  *out_pitch  = 1.0f;
        return;
    }

    /* Relative displacement vector (source - listener) */
    float dx = voice->px - listener->px;
    float dy = voice->py - listener->py;
    float dz = voice->pz - listener->pz;
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);

    /* 1. Distance Attenuation */
    float min_d = (voice->min_dist > 0.1f) ? voice->min_dist : 1.0f;
    float max_d = (voice->max_dist > min_d) ? voice->max_dist : 100.0f;
    float rolloff = (voice->rolloff > 0.0f) ? voice->rolloff : 1.0f;

    float att = 1.0f;
    if (dist <= min_d) {
        att = 1.0f;
    } else if (dist >= max_d) {
        att = 0.0f;
    } else {
        att = min_d / (min_d + rolloff * (dist - min_d));
    }

    /* 2. Equal-Power Stereo Panning */
    float pan = 0.0f;
    if (dist > 1e-4f) {
        float nx = dx / dist;
        float ny = dy / dist;
        float nz = dz / dist;
        /* Dot product with listener right vector */
        pan = nx * listener->rx + ny * listener->ry + nz * listener->rz;
        if (pan < -1.0f) pan = -1.0f;
        if (pan >  1.0f) pan =  1.0f;
    }

    /* Constant power panning angle theta in [0, pi/2] */
    constexpr float PI_OVER_4 = 0.78539816339f;
    float theta = PI_OVER_4 * (pan + 1.0f);
    float gain_l = cosf(theta) * att * voice->gain;
    float gain_r = sinf(theta) * att * voice->gain;

    /* 3. Doppler Shift */
    float eff_pitch = (voice->pitch > 0.05f) ? voice->pitch : 1.0f;
    if (dist > 1e-4f) {
        /* Unit vector from listener towards source */
        float nx = dx / dist;
        float ny = dy / dist;
        float nz = dz / dist;

        /* Observer velocity towards source: v_o = v_listener . d_hat */
        float v_o = listener->vx * nx + listener->vy * ny + listener->vz * nz;
        /* Source velocity towards observer: v_s = -v_source . d_hat */
        float v_s = -(voice->vx * nx + voice->vy * ny + voice->vz * nz);

        constexpr float MAX_V = KHR_AUDIO_SPEED_OF_SOUND * 0.8f;
        if (v_o >  MAX_V) v_o =  MAX_V;
        if (v_o < -MAX_V) v_o = -MAX_V;
        if (v_s >  MAX_V) v_s =  MAX_V;
        if (v_s < -MAX_V) v_s = -MAX_V;

        float doppler = (KHR_AUDIO_SPEED_OF_SOUND + v_o) / (KHR_AUDIO_SPEED_OF_SOUND - v_s);
        if (doppler < 0.2f) doppler = 0.2f;
        if (doppler > 4.0f) doppler = 4.0f;
        eff_pitch *= doppler;
    }

    if (out_gain_l) *out_gain_l = gain_l;
    if (out_gain_r) *out_gain_r = gain_r;
    if (out_pitch)  *out_pitch  = eff_pitch;
}

[[nodiscard]]
uint32_t khr_audio_mixer_play(khr_audio_mixer_t* mixer, const khr_audio_clip_t* clip,
                             float x, float y, float z, float gain, bool loop) {
    if (mixer == nullptr || clip == nullptr || clip->frame_count == 0 || clip->samples == nullptr) {
        return 0;
    }

    /* Find an inactive voice slot or steal the quietest */
    int slot = -1;
    float min_gain = 1e9f;
    int steal_slot = 0;

    for (uint32_t i = 0; i < KHR_AUDIO_MAX_VOICES; i++) {
        if (!mixer->voices[i].active) {
            slot = (int)i;
            break;
        }
        float cur_gain = mixer->voices[i].eff_gain_l + mixer->voices[i].eff_gain_r;
        if (cur_gain < min_gain) {
            min_gain = cur_gain;
            steal_slot = (int)i;
        }
    }

    if (slot < 0) {
        slot = steal_slot;
    }

    uint32_t id = mixer->next_voice_id++;
    if (mixer->next_voice_id == 0) {
        mixer->next_voice_id = 1;
    }

    khr_audio_voice_t* v = &mixer->voices[slot];
    memset(v, 0, sizeof(*v));
    v->voice_id  = id;
    v->clip      = clip;
    v->px        = x;
    v->py        = y;
    v->pz        = z;
    v->gain      = (gain >= 0.0f) ? gain : 1.0f;
    v->pitch     = 1.0f;
    v->min_dist  = 4.0f;
    v->max_dist  = 100.0f;
    v->rolloff   = 0.25f;
    v->playhead  = 0.0;
    v->loop      = loop;
    v->active    = true;
    v->paused    = false;

    khr_audio_calc_spatial(&mixer->listener, v, &v->eff_gain_l, &v->eff_gain_r, &v->eff_pitch);
    return id;
}

void khr_audio_mixer_stop(khr_audio_mixer_t* mixer, uint32_t voice_id) {
    if (mixer == nullptr || voice_id == 0) {
        return;
    }
    for (uint32_t i = 0; i < KHR_AUDIO_MAX_VOICES; i++) {
        if (mixer->voices[i].voice_id == voice_id) {
            mixer->voices[i].active = false;
            break;
        }
    }
}

void khr_audio_mixer_pause(khr_audio_mixer_t* mixer, uint32_t voice_id) {
    if (mixer == nullptr || voice_id == 0) {
        return;
    }
    for (uint32_t i = 0; i < KHR_AUDIO_MAX_VOICES; i++) {
        if (mixer->voices[i].voice_id == voice_id) {
            mixer->voices[i].paused = true;
            break;
        }
    }
}

void khr_audio_mixer_resume(khr_audio_mixer_t* mixer, uint32_t voice_id) {
    if (mixer == nullptr || voice_id == 0) {
        return;
    }
    for (uint32_t i = 0; i < KHR_AUDIO_MAX_VOICES; i++) {
        if (mixer->voices[i].voice_id == voice_id) {
            mixer->voices[i].paused = false;
            break;
        }
    }
}

void khr_audio_mixer_set_voice_pos(khr_audio_mixer_t* mixer, uint32_t voice_id,
                                  float x, float y, float z) {
    if (mixer == nullptr || voice_id == 0) {
        return;
    }
    for (uint32_t i = 0; i < KHR_AUDIO_MAX_VOICES; i++) {
        if (mixer->voices[i].voice_id == voice_id) {
            mixer->voices[i].px = x;
            mixer->voices[i].py = y;
            mixer->voices[i].pz = z;
            khr_audio_calc_spatial(&mixer->listener, &mixer->voices[i],
                                   &mixer->voices[i].eff_gain_l,
                                   &mixer->voices[i].eff_gain_r,
                                   &mixer->voices[i].eff_pitch);
            break;
        }
    }
}

void khr_audio_mixer_set_voice_vel(khr_audio_mixer_t* mixer, uint32_t voice_id,
                                  float vx, float vy, float vz) {
    if (mixer == nullptr || voice_id == 0) {
        return;
    }
    for (uint32_t i = 0; i < KHR_AUDIO_MAX_VOICES; i++) {
        if (mixer->voices[i].voice_id == voice_id) {
            mixer->voices[i].vx = vx;
            mixer->voices[i].vy = vy;
            mixer->voices[i].vz = vz;
            khr_audio_calc_spatial(&mixer->listener, &mixer->voices[i],
                                   &mixer->voices[i].eff_gain_l,
                                   &mixer->voices[i].eff_gain_r,
                                   &mixer->voices[i].eff_pitch);
            break;
        }
    }
}

void khr_audio_mixer_set_voice_gain(khr_audio_mixer_t* mixer, uint32_t voice_id, float gain) {
    if (mixer == nullptr || voice_id == 0) {
        return;
    }
    for (uint32_t i = 0; i < KHR_AUDIO_MAX_VOICES; i++) {
        if (mixer->voices[i].voice_id == voice_id) {
            mixer->voices[i].gain = (gain >= 0.0f) ? gain : 0.0f;
            khr_audio_calc_spatial(&mixer->listener, &mixer->voices[i],
                                   &mixer->voices[i].eff_gain_l,
                                   &mixer->voices[i].eff_gain_r,
                                   &mixer->voices[i].eff_pitch);
            break;
        }
    }
}

void khr_audio_mixer_set_voice_pitch(khr_audio_mixer_t* mixer, uint32_t voice_id, float pitch) {
    if (mixer == nullptr || voice_id == 0) {
        return;
    }
    for (uint32_t i = 0; i < KHR_AUDIO_MAX_VOICES; i++) {
        if (mixer->voices[i].voice_id == voice_id) {
            mixer->voices[i].pitch = (pitch > 0.05f) ? pitch : 1.0f;
            khr_audio_calc_spatial(&mixer->listener, &mixer->voices[i],
                                   &mixer->voices[i].eff_gain_l,
                                   &mixer->voices[i].eff_gain_r,
                                   &mixer->voices[i].eff_pitch);
            break;
        }
    }
}

uint32_t khr_audio_mix_chunk_s16(khr_audio_mixer_t* mixer, int16_t* out_pcm, uint32_t frames) {
    if (mixer == nullptr || out_pcm == nullptr || frames == 0) {
        return 0;
    }
    if (frames > KHR_AUDIO_MIX_CHUNK_CAP) {
        frames = KHR_AUDIO_MIX_CHUNK_CAP;
    }

    /* 1. Clear accumulation buffers */
    memset(mixer->accum_l, 0, (size_t)frames * sizeof(float));
    memset(mixer->accum_r, 0, (size_t)frames * sizeof(float));

    uint32_t active_voices = 0;

    /* 2. Mix each active voice */
    for (uint32_t v_idx = 0; v_idx < KHR_AUDIO_MAX_VOICES; v_idx++) {
        khr_audio_voice_t* v = &mixer->voices[v_idx];
        if (!v->active || v->paused || v->clip == nullptr || v->clip->samples == nullptr) {
            continue;
        }

        /* Update spatial positioning and gains per chunk */
        khr_audio_calc_spatial(&mixer->listener, v, &v->eff_gain_l, &v->eff_gain_r, &v->eff_pitch);

        float gl = v->eff_gain_l;
        float gr = v->eff_gain_r;
        if (gl <= 1e-6f && gr <= 1e-6f) {
            continue;
        }

        const khr_audio_clip_t* clip = v->clip;
        uint32_t clip_frames = clip->frame_count;
        uint32_t clip_sr = (clip->sample_rate > 0) ? clip->sample_rate : mixer->sample_rate;
        uint32_t channels = (clip->channels > 0) ? clip->channels : 1U;
        double rate_ratio = ((double)clip_sr / (double)mixer->sample_rate) * (double)v->eff_pitch;
        if (rate_ratio < 1e-4) {
            rate_ratio = 1e-4;
        }

        active_voices++;

#if defined(__AVX2__)
        /* AVX2 Fast Path: unit pitch, mono source, 8-wide unrolled */
        if (fabs(rate_ratio - 1.0) < 1e-5 && channels == 1 && (v->playhead == floor(v->playhead))) {
            uint32_t cur_pos = (uint32_t)v->playhead;
            uint32_t i = 0;
            __m256 vgl = _mm256_set1_ps(gl);
            __m256 vgr = _mm256_set1_ps(gr);

            while (i < frames && cur_pos < clip_frames) {
                uint32_t rem_clip = clip_frames - cur_pos;
                uint32_t rem_out  = frames - i;
                uint32_t chunk_len = (rem_clip < rem_out) ? rem_clip : rem_out;
                uint32_t chunk_avx = chunk_len & ~7U;

                for (uint32_t k = 0; k < chunk_avx; k += 8) {
                    __m256 s = _mm256_loadu_ps(&clip->samples[cur_pos + k]);
                    __m256 al = _mm256_loadu_ps(&mixer->accum_l[i + k]);
                    __m256 ar = _mm256_loadu_ps(&mixer->accum_r[i + k]);
                    al = _mm256_fmadd_ps(s, vgl, al);
                    ar = _mm256_fmadd_ps(s, vgr, ar);
                    _mm256_storeu_ps(&mixer->accum_l[i + k], al);
                    _mm256_storeu_ps(&mixer->accum_r[i + k], ar);
                }

                for (uint32_t k = chunk_avx; k < chunk_len; k++) {
                    float s = clip->samples[cur_pos + k];
                    mixer->accum_l[i + k] += s * gl;
                    mixer->accum_r[i + k] += s * gr;
                }

                i += chunk_len;
                cur_pos += chunk_len;

                if (cur_pos >= clip_frames) {
                    if (v->loop) {
                        cur_pos = 0;
                    } else {
                        v->active = false;
                        break;
                    }
                }
            }
            v->playhead = (double)cur_pos;
            continue;
        }
#endif

        /* General Resampling Path: linear interpolation with fractional playhead */
        for (uint32_t i = 0; i < frames; i++) {
            if (v->playhead >= (double)clip_frames) {
                if (v->loop) {
                    v->playhead = fmod(v->playhead, (double)clip_frames);
                } else {
                    v->active = false;
                    break;
                }
            }

            uint32_t idx0 = (uint32_t)v->playhead;
            uint32_t idx1 = (idx0 + 1U < clip_frames) ? (idx0 + 1U) : (v->loop ? 0U : idx0);
            float frac = (float)(v->playhead - (double)idx0);

            float s_l = 0.0f;
            float s_r = 0.0f;

            if (channels == 1) {
                float smp = (1.0f - frac) * clip->samples[idx0] + frac * clip->samples[idx1];
                s_l = smp * gl;
                s_r = smp * gr;
            } else {
                float smp_l = (1.0f - frac) * clip->samples[idx0 * 2U + 0U] + frac * clip->samples[idx1 * 2U + 0U];
                float smp_r = (1.0f - frac) * clip->samples[idx0 * 2U + 1U] + frac * clip->samples[idx1 * 2U + 1U];
                s_l = smp_l * gl;
                s_r = smp_r * gr;
            }

            mixer->accum_l[i] += s_l;
            mixer->accum_r[i] += s_r;
            v->playhead += rate_ratio;
        }

        if (!v->loop && v->playhead >= (double)clip_frames) {
            v->active = false;
        }
    }

    /* 3. SIMD Master Limiter & Conversion to Interleaved S16 PCM */
    uint32_t i = 0;

#if defined(__AVX2__)
    __m256 vmin   = _mm256_set1_ps(-1.0f);
    __m256 vmax   = _mm256_set1_ps(1.0f);
    __m256 vscale = _mm256_set1_ps(32767.0f);

    uint32_t frames_avx = frames & ~7U;
    for (; i < frames_avx; i += 8) {
        __m256 l = _mm256_load_ps(&mixer->accum_l[i]);
        __m256 r = _mm256_load_ps(&mixer->accum_r[i]);

        /* Clamp [-1.0, +1.0] and scale to 32767 */
        l = _mm256_max_ps(vmin, _mm256_min_ps(vmax, l));
        r = _mm256_max_ps(vmin, _mm256_min_ps(vmax, r));
        l = _mm256_mul_ps(l, vscale);
        r = _mm256_mul_ps(r, vscale);

        /* Interleave into 16-bit output */
        alignas(32) float l_arr[8];
        alignas(32) float r_arr[8];
        _mm256_store_ps(l_arr, l);
        _mm256_store_ps(r_arr, r);

        for (uint32_t k = 0; k < 8; k++) {
            out_pcm[(i + k) * 2U + 0U] = (int16_t)l_arr[k];
            out_pcm[(i + k) * 2U + 1U] = (int16_t)r_arr[k];
        }
    }
#endif

    /* Scalar remainder */
    for (; i < frames; i++) {
        float l = mixer->accum_l[i];
        float r = mixer->accum_r[i];

        if (l >  1.0f) l =  1.0f;
        if (l < -1.0f) l = -1.0f;
        if (r >  1.0f) r =  1.0f;
        if (r < -1.0f) r = -1.0f;

        out_pcm[i * 2U + 0U] = (int16_t)(l * 32767.0f);
        out_pcm[i * 2U + 1U] = (int16_t)(r * 32767.0f);
    }

    return active_voices;
}

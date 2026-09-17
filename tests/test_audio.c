#include "test_framework.h"
#include "khoros/audio/audio.h"
#include "khoros/audio/alsa.h"
#include "khoros/audio/mixer.h"

#include <math.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

[[nodiscard]]
bool test_audio_clip_and_pool_contracts(void) {
    TEST_ASSERT_EQ(sizeof(khr_audio_listener_t), 60U, "khr_audio_listener_t size contract");
    TEST_ASSERT_GE(sizeof(khr_audio_voice_t), 64U, "khr_audio_voice_t size >= 64");
    TEST_ASSERT_EQ(KHR_AUDIO_SPEED_OF_SOUND, 343.3f, "Speed of sound 343.3 m/s");

    /* Create test sine wave clip */
    constexpr uint32_t CLIP_FRAMES = 480; /* 10 ms at 48 kHz */
    alignas(64) float samples[CLIP_FRAMES] = {};
    for (uint32_t i = 0; i < CLIP_FRAMES; i++) {
        samples[i] = sinf(2.0f * (float)M_PI * 440.0f * (float)i / 48'000.0f);
    }

    khr_audio_clip_t clip = {
        .id          = 1,
        .sample_rate = 48'000,
        .channels    = 1,
        .frame_count = CLIP_FRAMES,
        .samples     = samples,
        .bda_address = 0x1000'0000ULL,
    };

    TEST_ASSERT_EQ(clip.sample_rate, 48'000U, "sample_rate 48 kHz");
    TEST_ASSERT_EQ(clip.channels, 1U, "mono clip");
    TEST_ASSERT_EQ(clip.frame_count, CLIP_FRAMES, "frame count match");
    TEST_ASSERT_NOT_NULL(clip.samples, "samples not null");

    return true;
}

[[nodiscard]]
bool test_audio_spatial_attenuation_and_panning(void) {
    khr_audio_listener_t listener = {
        .px = 0.0f, .py = 0.0f, .pz = 0.0f,
        .vx = 0.0f, .vy = 0.0f, .vz = 0.0f,
        .fx = 0.0f, .fy = 0.0f, .fz = -1.0f,
        .ux = 0.0f, .uy = 1.0f, .uz = 0.0f,
        .rx = 1.0f, .ry = 0.0f, .rz = 0.0f,
    };

    /* 1. Directly ahead: (0, 0, -5) -> centered, equal left/right gain */
    khr_audio_voice_t v_center = {
        .px = 0.0f, .py = 0.0f, .pz = -5.0f,
        .gain = 1.0f, .pitch = 1.0f,
        .min_dist = 1.0f, .max_dist = 50.0f, .rolloff = 1.0f,
    };
    float gl = 0.0f, gr = 0.0f, pitch = 0.0f;
    khr_audio_calc_spatial(&listener, &v_center, &gl, &gr, &pitch);
    TEST_ASSERT_GT(gl, 0.0f, "gain_l > 0");
    TEST_ASSERT_GT(gr, 0.0f, "gain_r > 0");
    TEST_ASSERT_LT(fabsf(gl - gr), 0.001f, "gl == gr for centered voice");

    /* 2. Hard right: (+10, 0, 0) -> right gain dominant */
    khr_audio_voice_t v_right = {
        .px = 10.0f, .py = 0.0f, .pz = 0.0f,
        .gain = 1.0f, .pitch = 1.0f,
        .min_dist = 1.0f, .max_dist = 50.0f, .rolloff = 1.0f,
    };
    khr_audio_calc_spatial(&listener, &v_right, &gl, &gr, &pitch);
    TEST_ASSERT_GT(gr, gl * 5.0f, "gr >> gl for hard right source");

    /* 3. Hard left: (-10, 0, 0) -> left gain dominant */
    khr_audio_voice_t v_left = {
        .px = -10.0f, .py = 0.0f, .pz = 0.0f,
        .gain = 1.0f, .pitch = 1.0f,
        .min_dist = 1.0f, .max_dist = 50.0f, .rolloff = 1.0f,
    };
    khr_audio_calc_spatial(&listener, &v_left, &gl, &gr, &pitch);
    TEST_ASSERT_GT(gl, gr * 5.0f, "gl >> gr for hard left source");

    /* 4. Beyond max distance: (0, 0, -200) -> silent */
    khr_audio_voice_t v_far = {
        .px = 0.0f, .py = 0.0f, .pz = -200.0f,
        .gain = 1.0f, .pitch = 1.0f,
        .min_dist = 1.0f, .max_dist = 50.0f, .rolloff = 1.0f,
    };
    khr_audio_calc_spatial(&listener, &v_far, &gl, &gr, &pitch);
    TEST_ASSERT_EQ(gl, 0.0f, "gl is 0.0 past max_dist");
    TEST_ASSERT_EQ(gr, 0.0f, "gr is 0.0 past max_dist");

    return true;
}

[[nodiscard]]
bool test_audio_doppler_frequency_shift(void) {
    khr_audio_listener_t listener = {
        .px = 0.0f, .py = 0.0f, .pz = 0.0f,
        .vx = 0.0f, .vy = 0.0f, .vz = 0.0f,
        .fx = 0.0f, .fy = 0.0f, .fz = -1.0f,
        .ux = 0.0f, .uy = 1.0f, .uz = 0.0f,
        .rx = 1.0f, .ry = 0.0f, .rz = 0.0f,
    };

    /* Approaching source along -Z: voice at (0, 0, -100) moving at +100 m/s towards listener */
    khr_audio_voice_t v_approaching = {
        .px = 0.0f, .py = 0.0f, .pz = -100.0f,
        .vx = 0.0f, .vy = 0.0f, .vz = 100.0f,
        .gain = 1.0f, .pitch = 1.0f,
        .min_dist = 1.0f, .max_dist = 500.0f, .rolloff = 1.0f,
    };
    float gl = 0.0f, gr = 0.0f, pitch_approach = 0.0f;
    khr_audio_calc_spatial(&listener, &v_approaching, &gl, &gr, &pitch_approach);
    TEST_ASSERT_GT(pitch_approach, 1.2f, "Approaching pitch shifted up");

    /* Receding source: voice at (0, 0, 100) moving at +100 m/s away from listener */
    khr_audio_voice_t v_receding = {
        .px = 0.0f, .py = 0.0f, .pz = 100.0f,
        .vx = 0.0f, .vy = 0.0f, .vz = 100.0f,
        .gain = 1.0f, .pitch = 1.0f,
        .min_dist = 1.0f, .max_dist = 500.0f, .rolloff = 1.0f,
    };
    float pitch_recede = 0.0f;
    khr_audio_calc_spatial(&listener, &v_receding, &gl, &gr, &pitch_recede);
    TEST_ASSERT_LT(pitch_recede, 0.85f, "Receding pitch shifted down");
    TEST_ASSERT_GT(pitch_approach, pitch_recede, "Approaching pitch > Receding pitch");

    /* Supersonic speed clamp safety: voice moving at 1000 m/s */
    khr_audio_voice_t v_supersonic = {
        .px = 0.0f, .py = 0.0f, .pz = -100.0f,
        .vx = 0.0f, .vy = 0.0f, .vz = 1'000.0f,
        .gain = 1.0f, .pitch = 1.0f,
        .min_dist = 1.0f, .max_dist = 500.0f, .rolloff = 1.0f,
    };
    float pitch_supersonic = 0.0f;
    khr_audio_calc_spatial(&listener, &v_supersonic, &gl, &gr, &pitch_supersonic);
    TEST_ASSERT_LE(pitch_supersonic, 4.0f, "Doppler clamped <= 4.0 max");
    TEST_ASSERT_GE(pitch_supersonic, 0.2f, "Doppler clamped >= 0.2 min");
    TEST_ASSERT(!isnan(pitch_supersonic), "Pitch is not NaN");

    return true;
}

[[nodiscard]]
bool test_audio_mixer_polyphony_and_simd_vectorization(void) {
    khr_audio_mixer_t mixer = {};
    khr_audio_mixer_init(&mixer, 48'000);

    /* Generate 3 distinct clips */
    constexpr uint32_t FRAMES = 512;
    alignas(64) float sine_samples[FRAMES] = {};
    alignas(64) float square_samples[FRAMES] = {};
    for (uint32_t i = 0; i < FRAMES; i++) {
        sine_samples[i] = sinf(2.0f * (float)M_PI * 440.0f * (float)i / 48'000.0f);
        square_samples[i] = (sine_samples[i] >= 0.0f) ? 0.8f : -0.8f;
    }

    khr_audio_clip_t clip_sine = {
        .id = 1, .sample_rate = 48'000, .channels = 1,
        .frame_count = FRAMES, .samples = sine_samples,
    };
    khr_audio_clip_t clip_square = {
        .id = 2, .sample_rate = 48'000, .channels = 1,
        .frame_count = FRAMES, .samples = square_samples,
    };

    /* Play 4 active voices at different positions */
    uint32_t v1 = khr_audio_mixer_play(&mixer, &clip_sine, -2.0f, 0.0f, -2.0f, 0.8f, true);
    uint32_t v2 = khr_audio_mixer_play(&mixer, &clip_square, 2.0f, 0.0f, -2.0f, 0.8f, true);
    uint32_t v3 = khr_audio_mixer_play(&mixer, &clip_sine, 0.0f, 0.0f, -5.0f, 0.5f, true);
    uint32_t v4 = khr_audio_mixer_play(&mixer, &clip_square, 0.0f, 3.0f, -1.0f, 0.6f, true);

    TEST_ASSERT_NE(v1, 0U, "voice 1 played");
    TEST_ASSERT_NE(v2, 0U, "voice 2 played");
    TEST_ASSERT_NE(v3, 0U, "voice 3 played");
    TEST_ASSERT_NE(v4, 0U, "voice 4 played");

    /* Mix chunk */
    alignas(64) int16_t out_pcm[FRAMES * 2] = {};
    uint32_t active = khr_audio_mix_chunk_s16(&mixer, out_pcm, FRAMES);
    TEST_ASSERT_EQ(active, 4U, "4 voices mixed");

    /* Check that PCM output has non-zero amplitude */
    bool has_sound = false;
    for (uint32_t i = 0; i < FRAMES * 2; i++) {
        if (out_pcm[i] != 0) {
            has_sound = true;
            break;
        }
    }
    TEST_ASSERT(has_sound, "Mixed PCM contains audio data");

    /* Test pause and resume */
    khr_audio_mixer_pause(&mixer, v1);
    active = khr_audio_mix_chunk_s16(&mixer, out_pcm, FRAMES);
    TEST_ASSERT_EQ(active, 3U, "3 voices mixed after v1 pause");

    khr_audio_mixer_resume(&mixer, v1);
    active = khr_audio_mix_chunk_s16(&mixer, out_pcm, FRAMES);
    TEST_ASSERT_EQ(active, 4U, "4 voices mixed after v1 resume");

    /* Test stop */
    khr_audio_mixer_stop(&mixer, v2);
    active = khr_audio_mix_chunk_s16(&mixer, out_pcm, FRAMES);
    TEST_ASSERT_EQ(active, 3U, "3 voices mixed after v2 stop");

    khr_audio_mixer_destroy(&mixer);
    return true;
}

[[nodiscard]]
bool test_audio_engine_io_uring_direct_streaming(void) {
    int pipefds[2] = { -1, -1 };
    TEST_ASSERT_EQ(pipe2(pipefds, O_NONBLOCK | O_CLOEXEC), 0, "pipe2 created");

    khr_audio_config_t cfg = {
        .sample_rate    = 48'000,
        .period_frames  = 512,
        .custom_sink_fd = pipefds[1],
        .use_io_uring   = true,
    };

    khr_audio_engine_t engine = {};
    TEST_ASSERT(khr_audio_engine_init(&engine, &cfg), "khr_audio_engine_init");
    TEST_ASSERT(engine.ring_live, "ring_live true");
    TEST_ASSERT(engine.buffers_registered, "buffers_registered true");
    TEST_ASSERT_GE(engine.timer_fd, 0, "timer_fd >= 0");

    /* Generate test clip and play */
    constexpr uint32_t SAMPLES = 512;
    alignas(64) float audio_data[SAMPLES] = {};
    for (uint32_t i = 0; i < SAMPLES; i++) {
        audio_data[i] = 0.5f;
    }
    khr_audio_clip_t clip = {
        .id = 1, .sample_rate = 48'000, .channels = 1,
        .frame_count = SAMPLES, .samples = audio_data,
    };
    (void)khr_audio_mixer_play(&engine.mixer, &clip, 0.0f, 0.0f, -1.0f, 1.0f, true);

    /* Tick 4 audio chunks */
    for (uint32_t i = 0; i < 4; i++) {
        TEST_ASSERT(khr_audio_engine_tick(&engine), "khr_audio_engine_tick");
    }

    /* Verify data was written to pipe */
    size_t expected_bytes = 4 * 512 * 2 * sizeof(int16_t); /* 8192 bytes */
    uint8_t read_buf[9000] = {};
    ssize_t total_read = 0;
    while (total_read < (ssize_t)expected_bytes) {
        ssize_t r = read(pipefds[0], read_buf + total_read, sizeof(read_buf) - (size_t)total_read);
        if (r <= 0) break;
        total_read += r;
    }
    TEST_ASSERT_EQ((size_t)total_read, expected_bytes, "All 4 audio periods streamed via io_uring");

    khr_audio_engine_destroy(&engine);
    close(pipefds[0]);
    return true;
}

[[nodiscard]]
bool test_audio_sample_rate_conversion(void) {
    khr_audio_mixer_t mixer = {};
    khr_audio_mixer_init(&mixer, 48'000);

    /* 44.1 kHz clip played on a 48.0 kHz mixer (standard CD audio format) */
    constexpr uint32_t CLIP_LEN = 441;
    alignas(64) float cd_samples[CLIP_LEN] = {};
    for (uint32_t i = 0; i < CLIP_LEN; i++) {
        cd_samples[i] = 0.7f;
    }

    khr_audio_clip_t clip_cd = {
        .id          = 10,
        .sample_rate = 44'100,
        .channels    = 1,
        .frame_count = CLIP_LEN,
        .samples     = cd_samples,
    };

    uint32_t v = khr_audio_mixer_play(&mixer, &clip_cd, 0.0f, 0.0f, -1.0f, 1.0f, false);
    TEST_ASSERT_NE(v, 0U, "44.1 kHz voice played");

    /* Mix 512 frames at 48 kHz (one full period, ~10.66 ms) */
    alignas(64) int16_t out_pcm[512 * 2] = {};
    uint32_t active = khr_audio_mix_chunk_s16(&mixer, out_pcm, 512);
    /* At rate_ratio = 44100 / 48000 = 0.91875, 512 mixer frames corresponds to 470.4 clip frames.
     * The non-looping 441-frame clip finishes and deactivates. */
    TEST_ASSERT_EQ(active, 1U, "Active voice during playback");
    TEST_ASSERT_EQ(mixer.voices[0].active, false, "Voice deactivated after 441 frames consumed");

    khr_audio_mixer_destroy(&mixer);
    return true;
}

[[nodiscard]]
bool test_audio_unaligned_clip_loop_avx2(void) {
    khr_audio_mixer_t mixer = {};
    khr_audio_mixer_init(&mixer, 48'000);

    /* Clip with 10 frames (10 % 8 != 0) played looping under AVX2 fast path */
    constexpr uint32_t UNALIGNED_FRAMES = 10;
    alignas(64) float samples[UNALIGNED_FRAMES] = {};
    for (uint32_t i = 0; i < UNALIGNED_FRAMES; i++) {
        samples[i] = 0.6f;
    }

    khr_audio_clip_t clip = {
        .id          = 20,
        .sample_rate = 48'000,
        .channels    = 1,
        .frame_count = UNALIGNED_FRAMES,
        .samples     = samples,
    };

    uint32_t v = khr_audio_mixer_play(&mixer, &clip, 0.0f, 0.0f, -1.0f, 1.0f, true);
    TEST_ASSERT_NE(v, 0U, "Unaligned loop voice played");

    /* Mix 512 frames (51+ full loops across multiple chunk boundaries) */
    alignas(64) int16_t out_pcm[512 * 2] = {};
    uint32_t active = khr_audio_mix_chunk_s16(&mixer, out_pcm, 512);
    TEST_ASSERT_EQ(active, 1U, "1 voice active");
    TEST_ASSERT_NE(out_pcm[0], 0, "Loop output contains non-zero PCM");
    TEST_ASSERT_NE(out_pcm[511 * 2], 0, "End of chunk contains non-zero PCM");

    khr_audio_mixer_destroy(&mixer);
    return true;
}

[[nodiscard]]
bool test_audio_zero_sample_rate_fallback(void) {
    khr_audio_mixer_t mixer = {};
    khr_audio_mixer_init(&mixer, 48'000);

    constexpr uint32_t CLIP_LEN = 100;
    alignas(64) float samples[CLIP_LEN] = {};
    for (uint32_t i = 0; i < CLIP_LEN; i++) {
        samples[i] = 0.5f;
    }

    /* Clip with sample_rate = 0 (unspecified) should default to mixer rate and finish */
    khr_audio_clip_t clip = {
        .id          = 30,
        .sample_rate = 0,
        .channels    = 0,
        .frame_count = CLIP_LEN,
        .samples     = samples,
    };

    uint32_t v = khr_audio_mixer_play(&mixer, &clip, 0.0f, 0.0f, -1.0f, 1.0f, false);
    TEST_ASSERT_NE(v, 0U, "Zero sample rate voice played");

    alignas(64) int16_t out_pcm[150 * 2] = {};
    (void)khr_audio_mix_chunk_s16(&mixer, out_pcm, 150);
    TEST_ASSERT_EQ(mixer.voices[0].active, false, "Voice cleanly terminated without infinite hang");

    khr_audio_mixer_destroy(&mixer);
    return true;
}

[[nodiscard]]
bool test_audio_pipe_backend_auto_routing(void) {
    /* 1. Test null sink explicitly disabled */
    khr_audio_config_t cfg_null = {
        .sample_rate = 48'000,
        .period_frames = 512,
        .disabled = true,
    };
    khr_audio_engine_t eng_null = {};
    TEST_ASSERT(khr_audio_engine_init(&eng_null, &cfg_null), "init null engine");
    TEST_ASSERT_EQ((int)eng_null.active_backend, (int)KHR_AUDIO_BACKEND_NULL, "active backend null");
    const char* desc = khr_audio_engine_get_backend_name(&eng_null);
    TEST_ASSERT(desc != nullptr && strstr(desc, "Null") != nullptr, "null desc contains Null");
    khr_audio_engine_destroy(&eng_null);

    /* 2. Test auto backend selection (PipeWire pipe or fallback) */
    khr_audio_config_t cfg_auto = {
        .sample_rate = 48'000,
        .period_frames = 512,
        .backend = KHR_AUDIO_BACKEND_AUTO,
    };
    khr_audio_engine_t eng_auto = {};
    TEST_ASSERT(khr_audio_engine_init(&eng_auto, &cfg_auto), "init auto engine");
    const char* auto_desc = khr_audio_engine_get_backend_name(&eng_auto);
    TEST_ASSERT(auto_desc != nullptr && strlen(auto_desc) > 0, "auto desc valid");
    TEST_ASSERT(khr_audio_engine_tick(&eng_auto), "tick auto engine");
    khr_audio_engine_destroy(&eng_auto);

    return true;
}

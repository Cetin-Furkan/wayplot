#include "khoros/audio/audio.h"

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/timerfd.h>
#include "khoros/core/cpu.h"

static void* audio_worker_thread(void* arg) {
    khr_audio_engine_t* engine = (khr_audio_engine_t*)arg;
    if (engine == nullptr) {
        return nullptr;
    }

    uint64_t period_ns = (uint64_t)engine->period_frames * 1'000'000'000ULL / (uint64_t)engine->sample_rate;
    struct timespec deadline = {};
    clock_gettime(CLOCK_MONOTONIC, &deadline);

    while (atomic_load_explicit(&engine->running, memory_order_acquire)) {
        if (engine->timer_fd >= 0) {
            uint64_t expirations = 0;
            ssize_t s = read(engine->timer_fd, &expirations, sizeof(expirations));
            if (s <= 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (!atomic_load_explicit(&engine->running, memory_order_acquire)) {
                break;
            }
        } else {
            /* Fallback clock deadline */
            deadline.tv_nsec += (long)period_ns;
            if (deadline.tv_nsec >= 1'000'000'000L) {
                deadline.tv_sec += deadline.tv_nsec / 1'000'000'000L;
                deadline.tv_nsec %= 1'000'000'000L;
            }
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr);
            if (!atomic_load_explicit(&engine->running, memory_order_acquire)) {
                break;
            }
        }

        (void)khr_audio_engine_tick(engine);
    }

    return nullptr;
}

[[nodiscard]]
bool khr_audio_engine_init(khr_audio_engine_t* engine, const khr_audio_config_t* cfg) {
    if (engine == nullptr) {
        return false;
    }
    memset(engine, 0, sizeof(*engine));

    engine->sample_rate   = (cfg && cfg->sample_rate > 0) ? cfg->sample_rate : KHR_AUDIO_DEFAULT_SAMPLE_RATE;
    engine->period_frames = (cfg && cfg->period_frames > 0) ? cfg->period_frames : KHR_AUDIO_CHUNK_FRAMES;
    engine->chunk_bytes   = (size_t)engine->period_frames * 2U * sizeof(int16_t);
    engine->timer_fd      = -1;

    /* 1. Open ALSA hardware device or custom/fallback sink */
    bool opened = false;
    if (cfg && cfg->custom_sink_fd >= 0) {
        opened = khr_alsa_pcm_open_mock(&engine->alsa, cfg->custom_sink_fd,
                                        engine->sample_rate, 2U, engine->period_frames);
    } else {
        uint32_t card = cfg ? cfg->alsa_card : 0U;
        uint32_t device = cfg ? cfg->alsa_device : 0U;
        opened = khr_alsa_pcm_open(&engine->alsa, card, device,
                                   engine->sample_rate, 2U, engine->period_frames);
        if (!opened) {
            /* Fallback to null sink for headless environments without ALSA audio */
            int null_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
            if (null_fd >= 0) {
                opened = khr_alsa_pcm_open_mock(&engine->alsa, null_fd,
                                                engine->sample_rate, 2U, engine->period_frames);
            }
        }
    }

    if (!opened) {
        return false;
    }

    /* 2. Initialize 3D Audio Mixer */
    khr_audio_mixer_init(&engine->mixer, engine->sample_rate);

    /* 3. Setup io_uring direct buffer registration if requested */
    bool use_uring = cfg ? cfg->use_io_uring : false;
    if (use_uring) {
        khr_uring_config_t rcfg = {
            .sq_entries = 16,
            .cq_entries = 32,
            .flags      = 0,
        };
        if (khr_uring_init(&engine->ring, &rcfg)) {
            engine->ring_live = true;

            struct iovec iov[KHR_AUDIO_RING_BUFFER_COUNT] = {};
            for (uint32_t i = 0; i < KHR_AUDIO_RING_BUFFER_COUNT; i++) {
                iov[i].iov_base = engine->chunk_buffers[i];
                iov[i].iov_len  = sizeof(engine->chunk_buffers[i]);
            }

            if (khr_uring_register_buffers(&engine->ring, iov, KHR_AUDIO_RING_BUFFER_COUNT)) {
                engine->buffers_registered = true;
            }
        }
    }

    /* 4. Setup timerfd for real-time periodic pacing */
    engine->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (engine->timer_fd >= 0) {
        uint64_t period_ns = (uint64_t)engine->period_frames * 1'000'000'000ULL / (uint64_t)engine->sample_rate;
        struct itimerspec its = {
            .it_interval = {
                .tv_sec  = (time_t)(period_ns / 1'000'000'000ULL),
                .tv_nsec = (long)(period_ns % 1'000'000'000ULL),
            },
            .it_value = {
                .tv_sec  = (time_t)(period_ns / 1'000'000'000ULL),
                .tv_nsec = (long)(period_ns % 1'000'000'000ULL),
            },
        };
        (void)timerfd_settime(engine->timer_fd, 0, &its, nullptr);
    }

    pthread_mutex_init(&engine->lock, nullptr);
    atomic_init(&engine->running, false);
    atomic_init(&engine->worker_started, false);
    return true;
}

void khr_audio_engine_destroy(khr_audio_engine_t* engine) {
    if (engine == nullptr) {
        return;
    }

    khr_audio_engine_stop_worker(engine);
    pthread_mutex_destroy(&engine->lock);

    if (engine->timer_fd >= 0) {
        close(engine->timer_fd);
        engine->timer_fd = -1;
    }

    if (engine->ring_live) {
        struct io_uring_cqe* cqe = nullptr;
        while (khr_uring_peek_cqe(&engine->ring, &cqe)) {
            khr_uring_cqe_seen(&engine->ring, cqe);
        }
        if (engine->buffers_registered) {
            (void)khr_uring_unregister_buffers(&engine->ring);
            engine->buffers_registered = false;
        }
        khr_uring_destroy(&engine->ring);
        engine->ring_live = false;
    }

    khr_alsa_pcm_close(&engine->alsa);
    khr_audio_mixer_destroy(&engine->mixer);
}

[[nodiscard]]
bool khr_audio_engine_tick(khr_audio_engine_t* engine) {
    if (engine == nullptr) {
        return false;
    }

    uint32_t buf_idx = engine->write_buffer_idx;
    int16_t* pcm_buf = engine->chunk_buffers[buf_idx];

    /* Mix audio chunk into current buffer */
    pthread_mutex_lock(&engine->lock);
    (void)khr_audio_mix_chunk_s16(&engine->mixer, pcm_buf, engine->period_frames);
    pthread_mutex_unlock(&engine->lock);

    /* Submit audio buffer */
    if (engine->ring_live && engine->buffers_registered && engine->alsa.is_mock) {
        /* Direct descriptor WRITE_FIXED submission on io_uring */
        struct io_uring_sqe* sqe = khr_uring_prep_write_fixed(&engine->ring,
                                                             engine->alsa.fd,
                                                             pcm_buf,
                                                             (uint32_t)engine->chunk_bytes,
                                                             0,
                                                             (uint16_t)buf_idx,
                                                             false,
                                                             0xAU);
        if (sqe != nullptr) {
            (void)khr_uring_submit(&engine->ring, 0);
            struct io_uring_cqe* cqe = nullptr;
            while (khr_uring_peek_cqe(&engine->ring, &cqe)) {
                khr_uring_cqe_seen(&engine->ring, cqe);
            }
        }
    } else {
        /* Direct PCM write to ALSA device */
        (void)khr_alsa_pcm_write(&engine->alsa, pcm_buf, engine->period_frames);
    }

    engine->write_buffer_idx = (buf_idx + 1U) % KHR_AUDIO_RING_BUFFER_COUNT;
    return true;
}

[[nodiscard]]
bool khr_audio_engine_start_worker(khr_audio_engine_t* engine) {
    if (engine == nullptr) {
        return false;
    }
    if (atomic_load_explicit(&engine->worker_started, memory_order_acquire)) {
        return true;
    }

    atomic_store_explicit(&engine->running, true, memory_order_release);
    if (pthread_create(&engine->worker, nullptr, audio_worker_thread, engine) != 0) {
        atomic_store_explicit(&engine->running, false, memory_order_release);
        return false;
    }

    atomic_store_explicit(&engine->worker_started, true, memory_order_release);
    return true;
}

void khr_audio_engine_stop_worker(khr_audio_engine_t* engine) {
    if (engine == nullptr) {
        return;
    }
    if (!atomic_load_explicit(&engine->worker_started, memory_order_acquire)) {
        return;
    }

    atomic_store_explicit(&engine->running, false, memory_order_release);
    if (engine->timer_fd >= 0) {
        struct itimerspec its = {
            .it_value = { .tv_sec = 0, .tv_nsec = 1 },
        };
        (void)timerfd_settime(engine->timer_fd, 0, &its, nullptr);
    }
    (void)pthread_join(engine->worker, nullptr);
    atomic_store_explicit(&engine->worker_started, false, memory_order_release);
}

[[nodiscard]]
uint32_t khr_audio_engine_play(khr_audio_engine_t* engine, const khr_audio_clip_t* clip,
                              float x, float y, float z, float gain, bool loop) {
    if (engine == nullptr || clip == nullptr) {
        return 0;
    }
    pthread_mutex_lock(&engine->lock);
    uint32_t id = khr_audio_mixer_play(&engine->mixer, clip, x, y, z, gain, loop);
    pthread_mutex_unlock(&engine->lock);
    return id;
}

void khr_audio_engine_set_listener(khr_audio_engine_t* engine, const khr_audio_listener_t* listener) {
    if (engine == nullptr || listener == nullptr) {
        return;
    }
    pthread_mutex_lock(&engine->lock);
    khr_audio_mixer_set_listener(&engine->mixer, listener);
    pthread_mutex_unlock(&engine->lock);
}

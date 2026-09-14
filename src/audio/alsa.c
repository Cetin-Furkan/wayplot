#include "khoros/audio/alsa.h"

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>

static void set_mask(struct snd_mask* mask, unsigned int val) {
    memset(mask, 0, sizeof(*mask));
    mask->bits[val / 32U] = 1U << (val % 32U);
}

static void set_interval(struct snd_interval* iv, unsigned int val) {
    memset(iv, 0, sizeof(*iv));
    iv->min = val;
    iv->max = val;
    iv->integer = 1;
}

static void set_interval_range(struct snd_interval* iv, unsigned int min_val, unsigned int max_val) {
    memset(iv, 0, sizeof(*iv));
    iv->min = min_val;
    iv->max = max_val;
    iv->integer = 1;
}

[[nodiscard]]
bool khr_alsa_pcm_open(khr_alsa_pcm_t* pcm, uint32_t card, uint32_t device,
                       uint32_t sample_rate, uint32_t channels, uint32_t period_frames) {
    if (pcm == nullptr) {
        return false;
    }
    memset(pcm, 0, sizeof(*pcm));
    pcm->fd = -1;

    char path[64] = {};
    (void)snprintf(path, sizeof(path), "/dev/snd/pcmC%uD%up", card, device);

    int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }

    /* 1. Query / Refine initial parameter space */
    struct snd_pcm_hw_params params = {};
    for (size_t i = 0; i < 12; i++) {
        params.intervals[i].min = 0;
        params.intervals[i].max = 0xFFFF'FFFFU;
    }
    for (size_t i = 0; i < 3; i++) {
        memset(&params.masks[i], 0xFF, sizeof(params.masks[i]));
    }

    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, &params) < 0) {
        close(fd);
        return false;
    }

    /* 2. Constrain HW params */
    set_mask(&params.masks[SNDRV_PCM_HW_PARAM_ACCESS - SNDRV_PCM_HW_PARAM_FIRST_MASK],
             SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    set_mask(&params.masks[SNDRV_PCM_HW_PARAM_FORMAT - SNDRV_PCM_HW_PARAM_FIRST_MASK],
             SNDRV_PCM_FORMAT_S16_LE);
    set_mask(&params.masks[SNDRV_PCM_HW_PARAM_SUBFORMAT - SNDRV_PCM_HW_PARAM_FIRST_MASK],
             SNDRV_PCM_SUBFORMAT_STD);

    set_interval(&params.intervals[SNDRV_PCM_HW_PARAM_CHANNELS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL],
                 channels);
    set_interval(&params.intervals[SNDRV_PCM_HW_PARAM_RATE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL],
                 sample_rate);
    set_interval(&params.intervals[SNDRV_PCM_HW_PARAM_PERIOD_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL],
                 period_frames);
    set_interval_range(&params.intervals[SNDRV_PCM_HW_PARAM_BUFFER_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL],
                       period_frames * 2U, period_frames * 8U);

    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &params) < 0) {
        close(fd);
        return false;
    }

    uint32_t chosen_period = params.intervals[SNDRV_PCM_HW_PARAM_PERIOD_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min;
    uint32_t chosen_buffer = params.intervals[SNDRV_PCM_HW_PARAM_BUFFER_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min;

    /* 3. Configure SW params */
    struct snd_pcm_sw_params sw = {
        .tstamp_mode     = SNDRV_PCM_TSTAMP_NONE,
        .period_step     = 1,
        .avail_min       = chosen_period,
        .start_threshold = chosen_period * 2U,
        .stop_threshold  = chosen_buffer,
    };
    if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw) < 0) {
        close(fd);
        return false;
    }

    /* 4. Transition stream to PREPARED state */
    if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
        close(fd);
        return false;
    }

    pcm->fd            = fd;
    pcm->is_mock       = false;
    pcm->is_prepared   = true;
    pcm->card          = card;
    pcm->device        = device;
    pcm->sample_rate   = sample_rate;
    pcm->channels      = channels;
    pcm->period_frames = chosen_period;
    pcm->buffer_frames = chosen_buffer;
    pcm->frame_bytes   = (size_t)channels * sizeof(int16_t);
    pcm->period_bytes  = (size_t)chosen_period * pcm->frame_bytes;
    pcm->buffer_bytes  = (size_t)chosen_buffer * pcm->frame_bytes;

    return true;
}

[[nodiscard]]
bool khr_alsa_pcm_open_mock(khr_alsa_pcm_t* pcm, int sink_fd,
                            uint32_t sample_rate, uint32_t channels, uint32_t period_frames) {
    if (pcm == nullptr || sink_fd < 0) {
        return false;
    }
    memset(pcm, 0, sizeof(*pcm));
    pcm->fd            = sink_fd;
    pcm->is_mock       = true;
    pcm->is_prepared   = true;
    pcm->sample_rate   = sample_rate;
    pcm->channels      = channels;
    pcm->period_frames = period_frames;
    pcm->buffer_frames = period_frames * 4U;
    pcm->frame_bytes   = (size_t)channels * sizeof(int16_t);
    pcm->period_bytes  = (size_t)period_frames * pcm->frame_bytes;
    pcm->buffer_bytes  = (size_t)pcm->buffer_frames * pcm->frame_bytes;
    return true;
}

[[nodiscard]]
bool khr_alsa_pcm_prepare(khr_alsa_pcm_t* pcm) {
    if (pcm == nullptr || pcm->fd < 0) {
        return false;
    }
    if (pcm->is_mock) {
        pcm->is_prepared = true;
        return true;
    }
    if (ioctl(pcm->fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
        return false;
    }
    pcm->is_prepared = true;
    return true;
}

[[nodiscard]]
int64_t khr_alsa_pcm_write(khr_alsa_pcm_t* pcm, const void* frames, uint32_t frame_count) {
    if (pcm == nullptr || pcm->fd < 0 || frames == nullptr || frame_count == 0) {
        return -EINVAL;
    }

    size_t total_bytes = (size_t)frame_count * pcm->frame_bytes;
    ssize_t written = write(pcm->fd, frames, total_bytes);
    if (written < 0) {
        int err = errno;
        /* Underrun (XRUN): recover via prepare and retry once */
        if (err == EPIPE) {
            (void)khr_alsa_pcm_prepare(pcm);
            written = write(pcm->fd, frames, total_bytes);
            if (written < 0) {
                return -errno;
            }
        } else if (err == EAGAIN) {
            return 0; /* Non-blocking buffer full */
        } else {
            return -err;
        }
    }

    return (int64_t)(written / (ssize_t)pcm->frame_bytes);
}

void khr_alsa_pcm_close(khr_alsa_pcm_t* pcm) {
    if (pcm == nullptr) {
        return;
    }
    if (pcm->fd >= 0) {
        close(pcm->fd);
        pcm->fd = -1;
    }
    pcm->is_prepared = false;
}

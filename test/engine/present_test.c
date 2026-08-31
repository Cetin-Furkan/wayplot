#define _GNU_SOURCE
#include "engine/present.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int g_fail;

static void expect(int cond, const char *what)
{
    if (cond)
        printf("PASS  %s\n", what);
    else {
        printf("FAIL  %s\n", what);
        g_fail++;
    }
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int main(void)
{
    struct wp_session s;
    struct wp_present p;
    struct wp_present_frame f;
    uint64_t deadline;
    unsigned presented = 0, polls = 0;
    int ret;

    ret = wp_session_open(&s);
    expect(ret == 0, "session_open");
    if (ret < 0)
        return 1;
    ret = wp_session_setup_surface(&s);
    expect(ret == 0, "xdg configure + dma-buf feedback");
    if (ret < 0) {
        wp_session_close(&s);
        return 1;
    }

    ret = wp_present_open(&p, &s);
    expect(ret == 0, "device + negotiate + 3 dma-buf images + two-role timelines");
    if (ret < 0) {
        printf("      present_open %s\n", strerror(-ret));
        wp_session_close(&s);
        return 1;
    }
    expect(p.sc.allocated && p.sc.images[0].dma_fd >= 0, "swapchain dma-buf exported");
    expect(p.sc.images[0].release_fd >= 0 && p.sc.images[1].release_fd >= 0 &&
           p.sc.images[2].release_fd >= 0,
           "per-image release timelines (protocol forbids sharing release across buffers)");
    expect(p.device.acquire_fd >= 0 && p.acquire_timeline != 0, "shared acquire timeline imported");
    expect(p.wl_buffer[0] && p.wl_buffer[1] && p.wl_buffer[2], "wl_buffer for each image");
    expect(p.negotiated.valid, "format/modifier intersection");

    deadline = now_ns() + 8000000000ull;
    while (presented < 8 && now_ns() < deadline && !s.closed) {
        polls++;
        ret = wp_present_poll(&p, 50ull * 1000ull * 1000ull);
        if (ret < 0) {
            printf("FAIL  present_poll %s\n", strerror(-ret));
            g_fail++;
            break;
        }
        if (!wp_present_begin(&p, &f))
            continue;
        ret = wp_present_end(&p, &f);
        if (ret < 0) {
            printf("FAIL  present_end %s\n", strerror(-ret));
            g_fail++;
            break;
        }
        presented++;
    }
    printf("      presented %u frames in %u polls  size %ux%u scale %d  modifier 0x%llx\n",
           presented, polls, p.sc.width, p.sc.height, s.scale,
           (unsigned long long)p.sc.params.chosen_modifier);
    expect(presented >= 3, "at least one present per swapchain image");
    expect(p.sc.width == s.buf_w && p.sc.height == s.buf_h,
           "swapchain is logical × scale, not a stretched logical buffer");
    expect(!s.closed || presented > 0, "no xdg close before first pixel unless user closed");

    {
        uint32_t want_w, want_h;
        unsigned got = 0;
        s.width = (s.width > 900) ? s.width - 128 : s.width + 128;
        if (s.width < 320)
            s.width = 640;
        wp_session_buffer_size(&s, &want_w, &want_h);
        s.buf_w = want_w;
        s.buf_h = want_h;
        s.size_dirty = true;
        s.configure_dirty = true;
        s.frame_done = true;
        deadline = now_ns() + 5000000000ull;
        while (got < 2 && now_ns() < deadline && !s.closed) {
            ret = wp_present_poll(&p, 50ull * 1000ull * 1000ull);
            if (ret < 0)
                break;
            if (!wp_present_begin(&p, &f))
                continue;
            ret = wp_present_end(&p, &f);
            if (ret < 0)
                break;
            got++;
        }
        printf("      after size_dirty swapchain %ux%u want %ux%u  frames %u\n",
               p.sc.width, p.sc.height, want_w, want_h, got);
        expect(got >= 1, "presented after a logical resize");
        expect(p.sc.width == want_w && p.sc.height == want_h,
               "retire+recreate produced DMA-BUFs at the new buffer size");
    }

    wp_present_close(&p);
    wp_session_close(&s);
    printf("%s  %d failure(s)\n", g_fail ? "RESULT FAIL" : "RESULT PASS", g_fail);
    return g_fail ? 1 : 0;
}

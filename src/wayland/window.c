#include "khoros/wayland/window.h"
#include "khoros/core/config.h"
#include "khoros/wayland/client.h"
#include "khoros/wayland/xdg.h"
#include "khoros/wayland/seat.h"
#include "khoros/wayland/cursor.h"
#include "khoros/wayland/dmabuf_present.h"
#include "khoros/wayland/wire.h"
#include "khoros/gfx/pipeline.h"
#include "khoros/uring/pbuf.h"

#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>

static volatile sig_atomic_t khr_window_stop = 0;

static void khr_window_on_sig(int sig) {
    (void)sig;
    khr_window_stop = 1;
}

[[nodiscard]]
static uint32_t khr_rgba8(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    return r | (g << 8) | (b << 16) | (a << 24);
}

static void khr_window_feed(khr_topology_t* topo, khr_wl_client_t* client,
                            khr_xdg_shell_t* shell, khr_seat_t* seat,
                            khr_dmabuf_present_t* dp, bool have_dp,
                            uint32_t wait_ms) {
    (void)khr_topology_pump(topo, wait_ms);
    bool rearm = false;
    for (;;) {
        khr_cqe_event_t evt = {};
        if (!khr_topology_pop_cqe(topo, &evt)) {
            break;
        }
        const uint8_t* data = nullptr;
        size_t len = 0;
        bool need_rearm = false;
        (void)khr_wl_client_feed_cqe(client, evt.user_data, evt.res, evt.flags,
                                     &data, &len, &need_rearm);
        if (need_rearm) {
            rearm = true;
        }
        if (data != nullptr && len >= 8) {
            (void)khr_xdg_consume(client, shell, data, len);
            (void)khr_seat_consume(client, seat, data, len);
            if (have_dp) {
                (void)khr_dmabuf_present_consume(dp, data, len);
            }
        }
        khr_topology_recycle_cqe_buffer(topo, &evt);
    }
    if (rearm || !client->inbound_armed) {
        (void)khr_wl_client_arm_inbound(client);
    }
}

static void khr_window_paint_cards(khr_card_instance_t* cards, uint32_t w,
                                   uint32_t h, bool fullscreen) {
    /* Stable colors. The old frame_no % 3 strobe looked like a fault
     * (near-black frames). */
    uint32_t body = khr_rgba8(48, 52, 64, 255);
    uint32_t bar = khr_rgba8(36, 36, 42, 255);
    float top = fullscreen ? 0.0f : (float)KHR_WINDOW_CHROME_TOP;
    float body_h = (float)h - top;
    if (body_h < 1.0f) {
        body_h = 1.0f;
    }
    cards[0] = (khr_card_instance_t){
        .rect = { 0.0f, top, (float)w, body_h },
        .bg_rgba = body,
        .border_rgba = body,
        .corner_radius = 0.0f,
        .border_width = 0.0f,
    };
    cards[1] = (khr_card_instance_t){
        .rect = { 0.0f, 0.0f, (float)w, fullscreen ? 0.0f : (float)KHR_WINDOW_CHROME_TOP },
        .bg_rgba = bar,
        .border_rgba = khr_rgba8(70, 70, 80, 255),
        .corner_radius = 0.0f,
        .border_width = fullscreen ? 0.0f : 1.0f,
    };
}

[[nodiscard]]
bool khr_window_run(khr_topology_t* topo, khr_gfx_device_t* dev,
                    khr_bda_arena_t* arena) {
    if (topo == nullptr || dev == nullptr || arena == nullptr) {
        return false;
    }
    khr_window_stop = 0;
    struct sigaction sa = {};
    sa.sa_handler = khr_window_on_sig;
    (void)sigaction(SIGINT, &sa, nullptr);
    (void)sigaction(SIGTERM, &sa, nullptr);

    khr_wl_client_t client = {};
    if (!khr_wl_client_connect(&client, &topo->ring_a, nullptr)) {
        printf("  Present:      skipped (no compositor socket)\n");
        return true;
    }
    khr_wl_client_attach_pbufs(&client, &topo->pbuf_tier0, &topo->pbuf_tier1);
    khr_wl_client_attach_topology(&client, topo);
    client.recv_hdr.msg_controllen = CMSG_SPACE(sizeof(int));
    if (!khr_wl_client_roundtrip(&client)) {
        printf("  Present:      FAILED registry roundtrip\n");
        khr_wl_client_disconnect(&client);
        return false;
    }
    bool have_globals =
        khr_wl_client_find_global(&client, "wl_compositor") != nullptr &&
        khr_wl_client_find_global(&client, "xdg_wm_base") != nullptr &&
        khr_wl_client_find_global(&client, "zwp_linux_dmabuf_v1") != nullptr &&
        khr_wl_client_find_global(&client, "wp_linux_drm_syncobj_manager_v1") != nullptr;
    printf("  Registry:     %u globals (compositor=%s xdg=%s dmabuf=%s syncobj=%s seat=%s cursor_shape=%s deco=%s)\n",
           client.globals_count,
           client.compositor_name ? "yes" : "no",
           client.xdg_wm_base_name ? "yes" : "no",
           client.dmabuf_name ? "yes" : "no",
           client.syncobj_manager_name ? "yes" : "no",
           khr_wl_client_find_global(&client, "wl_seat") ? "yes" : "no",
           khr_wl_client_find_global(&client, "wp_cursor_shape_manager_v1") ? "yes" : "no",
           khr_wl_client_find_global(&client, "zxdg_decoration_manager_v1") ? "yes" : "no");
    if (!have_globals) {
        printf("  Present:      skipped (missing required globals)\n");
        khr_wl_client_disconnect(&client);
        return true;
    }

    khr_xdg_shell_t shell = {};
    khr_xdg_init(&shell);
    khr_seat_t seat = {};
    khr_seat_init(&seat);
    khr_cursor_t cursor = {};
    khr_cursor_init(&cursor);
    if (!khr_xdg_bind(&client, &shell) ||
        !khr_xdg_create_toplevel(&client, &shell, "Khoros", "khoros-engine") ||
        !khr_xdg_request_csd(&client, &shell) ||
        !khr_xdg_set_min_size(&client, &shell, (int32_t)KHR_WINDOW_MIN_W,
                              (int32_t)KHR_WINDOW_MIN_H) ||
        !khr_seat_bind(&client, &seat) ||
        !khr_wl_client_arm_inbound(&client)) {
        printf("  Present:      FAILED xdg/seat setup\n");
        khr_wl_client_disconnect(&client);
        return false;
    }
    if (KHR_WINDOW_START_MAXIMIZED) {
        (void)khr_xdg_set_maximized(&client, &shell, true);
    }
    if (KHR_WINDOW_START_FULLSCREEN) {
        (void)khr_xdg_set_fullscreen(&client, &shell, true);
    }

    uint64_t t0 = 0;
    {
        struct timespec ts = {};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        t0 = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1'000'000ULL;
    }
    while (!shell.configured && !shell.closed && !client.display_error &&
           !khr_window_stop) {
        struct timespec now = {};
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t ms = (uint64_t)now.tv_sec * 1000ULL +
                      (uint64_t)now.tv_nsec / 1'000'000ULL;
        if (ms - t0 > 5'000ULL) {
            break;
        }
        khr_window_feed(topo, &client, &shell, &seat, nullptr, false, 50);
        (void)khr_seat_offer_devices(&client, &seat);
    }
    if (client.display_error) {
        printf("  Present:      DISPLAY ERROR object=%u code=%u '%s'\n",
               client.error_object, client.error_code, client.error_msg);
        khr_cursor_destroy(&client, &cursor);
        khr_wl_client_disconnect(&client);
        return false;
    }
    if (!khr_xdg_can_attach(&shell)) {
        printf("  Present:      FAILED attach gate\n");
        khr_cursor_destroy(&client, &cursor);
        khr_wl_client_disconnect(&client);
        return false;
    }
    (void)khr_seat_offer_devices(&client, &seat);
    (void)khr_cursor_setup(&client, shell.compositor_id, seat.pointer_id, &cursor);

    uint32_t buf_w = 0, buf_h = 0;
    khr_window_buffer_size(&shell, &buf_w, &buf_h);
    (void)khr_xdg_set_window_geometry(&client, &shell, 0, 0, (int32_t)buf_w,
                                      (int32_t)buf_h);
    printf("  XDG:          configured %dx%d (buffer %ux%u) serial=%u max=%d full=%d\n",
           shell.width, shell.height, buf_w, buf_h, shell.last_ack_serial,
           shell.maximized, shell.fullscreen);

    khr_card_instance_t* cards = nullptr;
    VkDeviceAddress cards_addr = 0;
    if (!khr_bda_arena_alloc(arena, sizeof(khr_card_instance_t) * 2U, 16,
                             (void**)&cards, &cards_addr)) {
        printf("  Present:      FAILED card BDA alloc\n");
        khr_cursor_destroy(&client, &cursor);
        khr_wl_client_disconnect(&client);
        return false;
    }

    khr_dmabuf_present_t dp = {};
    bool have_dp = false;
    bool ok = true;
    bool dirty = true;
    bool was_fullscreen = shell.fullscreen;

    while (!shell.closed && !client.display_error && !khr_window_stop) {
        bool gpu_pending = have_dp && dp.last_signaled < dev->acquire_point;
        uint32_t wait_ms = dirty ? 0U : (gpu_pending ? 1U : 500U);
        khr_window_feed(topo, &client, &shell, &seat, have_dp ? &dp : nullptr,
                        have_dp, wait_ms);
        (void)khr_seat_offer_devices(&client, &seat);
        if (seat.pointer_id != 0 && cursor.device_id == 0 && !cursor.shm_live) {
            (void)khr_cursor_setup(&client, shell.compositor_id, seat.pointer_id,
                                   &cursor);
        }

        uint32_t want_w = 0, want_h = 0;
        if (shell.width > 0 && shell.height > 0) {
            khr_window_buffer_size(&shell, &want_w, &want_h);
        } else if (have_dp) {
            want_w = dp.width;
            want_h = dp.height;
        } else {
            khr_window_buffer_size(&shell, &want_w, &want_h);
        }
        if (want_w == 0) {
            want_w = 1;
        }
        if (want_h == 0) {
            want_h = 1;
        }

        if (!have_dp) {
            khr_gfx_device_wait_idle(dev);
            if (!khr_dmabuf_present_init(dev, &client, shell.surface_id,
                                         want_w, want_h, &dp)) {
                printf("  Present:      FAILED dmabuf init %ux%u\n",
                       want_w, want_h);
                continue;
            }
            have_dp = true;
            buf_w = want_w;
            buf_h = want_h;
            dirty = true;
            (void)khr_xdg_set_window_geometry(&client, &shell, 0, 0,
                                              (int32_t)want_w, (int32_t)want_h);
            printf("  Present:      slots %ux%u\n", buf_w, buf_h);
        } else if (want_w != dp.width || want_h != dp.height) {
            /* Do not call present_init again: get_surface on a mapped
             * surface is already_constructed and the compositor kills us.
             * Recreate slots only; commit the new size; then drop the old
             * dma-bufs. */
            khr_gfx_device_wait_idle(dev);
            if (!khr_dmabuf_present_resize(dev, &dp, want_w, want_h)) {
                printf("  Present:      FAILED resize %ux%u\n",
                       want_w, want_h);
                continue;
            }
            buf_w = want_w;
            buf_h = want_h;
            dirty = true;
            (void)khr_xdg_set_window_geometry(&client, &shell, 0, 0,
                                              (int32_t)want_w, (int32_t)want_h);
            printf("  Present:      slots %ux%u\n", buf_w, buf_h);
        }

        khr_hit_t hit = khr_window_hit(seat.x, seat.y, buf_w, buf_h,
                                       shell.fullscreen);
        if (seat.pointer_in) {
            (void)khr_cursor_apply(&client, &cursor, seat.pointer_id,
                                   khr_seat_serial(&seat), hit, false);
        }
        if (seat.double_click && hit == KHR_HIT_MOVE && !shell.fullscreen) {
            (void)khr_xdg_set_maximized(&client, &shell, !shell.maximized);
            seat.left_down = false;
        } else if (seat.left_down && !shell.fullscreen) {
            uint32_t serial = khr_seat_serial(&seat);
            uint32_t edge = khr_hit_resize_edge(hit);
            if (hit == KHR_HIT_MOVE) {
                (void)khr_xdg_move(&client, &shell, seat.seat_id, serial);
            } else if (edge != KHR_XDG_RESIZE_NONE) {
                (void)khr_xdg_resize(&client, &shell, seat.seat_id, serial, edge);
            }
            seat.left_down = false;
        }
        if (seat.f11_pressed) {
            (void)khr_xdg_set_fullscreen(&client, &shell, !shell.fullscreen);
        }
        if (seat.esc_pressed && shell.fullscreen) {
            (void)khr_xdg_set_fullscreen(&client, &shell, false);
        }
        if (was_fullscreen != shell.fullscreen) {
            dirty = true;
            was_fullscreen = shell.fullscreen;
        }

        if (have_dp) {
            (void)khr_dmabuf_present_sync(dev, &dp);
        }
        bool size_ok = have_dp && dp.width == want_w && dp.height == want_h;
        if (size_ok && dirty && khr_dmabuf_present_next_free(&dp) != UINT32_MAX) {
            khr_window_paint_cards(cards, buf_w, buf_h, shell.fullscreen);
            if (khr_dmabuf_present_commit_cards(dev, &dp, cards_addr, 2)) {
                dirty = false;
                khr_dmabuf_present_drop_retired(dev, &dp);
            }
        }
    }

    if (client.display_error) {
        printf("  Error:        object=%u code=%u '%s'\n",
               client.error_object, client.error_code, client.error_msg);
        ok = false;
    }
    printf("  Present:      frames=%llu releases=%u size=%ux%u closed=%d stop=%d alive=%s\n",
           have_dp ? (unsigned long long)dp.frames : 0ULL,
           have_dp ? dp.releases : 0U, buf_w, buf_h, shell.closed,
           (int)khr_window_stop,
           (!client.display_error && (shell.closed || khr_window_stop)) ? "yes" : "no");

    if (have_dp) {
        khr_dmabuf_present_destroy(dev, &dp);
    }
    khr_cursor_destroy(&client, &cursor);
    khr_wl_client_disconnect(&client);
    if (ok && !client.display_error) {
        ok = true;
    }
    return ok;
}

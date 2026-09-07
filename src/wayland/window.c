#include "khoros/wayland/window.h"
#include "khoros/core/config.h"
#include "khoros/wayland/client.h"
#include "khoros/wayland/xdg.h"
#include "khoros/wayland/seat.h"
#include "khoros/wayland/cursor.h"
#include "khoros/wayland/dmabuf_present.h"
#include "khoros/wayland/shm.h"
#include "khoros/wayland/wire.h"
#include "khoros/gfx/pipeline.h"
#include "khoros/uring/pbuf.h"
#include <vulkan/vulkan.h>

#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <math.h>
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

static uint32_t khr_window_shm_argb(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    return b | (g << 8) | (r << 16) | (a << 24);
}

static void khr_window_paint_popup_shm(uint32_t* px, uint32_t w, uint32_t h) {
    const uint32_t bg = khr_window_shm_argb(28, 30, 38, 255);
    const uint32_t br = khr_window_shm_argb(90, 110, 150, 255);
    const int32_t r = 8;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            int32_t cx = (int32_t)x;
            int32_t cy = (int32_t)y;
            int32_t dx = 0;
            int32_t dy = 0;
            if (cx < r && cy < r) {
                dx = r - cx;
                dy = r - cy;
            } else if (cx >= (int32_t)w - r && cy < r) {
                dx = cx - ((int32_t)w - 1 - r);
                dy = r - cy;
            } else if (cx < r && cy >= (int32_t)h - r) {
                dx = r - cx;
                dy = cy - ((int32_t)h - 1 - r);
            } else if (cx >= (int32_t)w - r && cy >= (int32_t)h - r) {
                dx = cx - ((int32_t)w - 1 - r);
                dy = cy - ((int32_t)h - 1 - r);
            }
            if (dx > 0 && dy > 0 && dx * dx + dy * dy > r * r) {
                px[y * w + x] = 0;
                continue;
            }
            bool edge = x < 1 || y < 1 || x >= w - 1 || y >= h - 1 ||
                        (dx > 0 && dy > 0 && dx * dx + dy * dy > (r - 1) * (r - 1));
            px[y * w + x] = edge ? br : bg;
        }
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

static void khr_window_popup_teardown(khr_wl_client_t* client,
                                      khr_xdg_shell_t* shell,
                                      khr_shm_pool_t* pool,
                                      uint32_t* buffer_id) {
    khr_xdg_popup_destroy(client, shell);
    if (buffer_id != nullptr && *buffer_id != 0) {
        (void)khr_shm_buffer_destroy(client, *buffer_id);
        *buffer_id = 0;
    }
    khr_shm_pool_destroy(client, pool);
}

static void khr_window_wait_acquire(khr_gfx_device_t* dev) {
    if (dev == nullptr || dev->device == VK_NULL_HANDLE ||
        dev->acquire_sem == VK_NULL_HANDLE || dev->acquire_point == 0) {
        return;
    }
    uint64_t want = dev->acquire_point;
    VkSemaphoreWaitInfo wi = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores = &dev->acquire_sem,
        .pValues = &want,
    };
    (void)vkWaitSemaphores(dev->device, &wi, 50'000'000ULL);
}

static void khr_window_plot_push(khr_plot_push_t* push, VkDeviceAddress samples) {
    /* Column-major Ry(yaw)*Rx(pitch) so ribbon half_w is visible, not a line. */
    const float yaw = 0.42f;
    const float pitch = 0.32f;
    float cy = cosf(yaw);
    float sy = sinf(yaw);
    float cx = cosf(pitch);
    float sx = sinf(pitch);
    *push = (khr_plot_push_t){
        .mvp_c0 = { cy, 0.0f, -sy, 0.0f },
        .mvp_c1 = { sy * sx, cx, cy * sx, 0.0f },
        .mvp_c2 = { sy * cx, -sx, cy * cx, 0.0f },
        .mvp_c3 = { 0.0f, 0.0f, 0.0f, 1.0f },
        .light_dir = { 0.35f, -0.80f, -0.50f, 0.0f },
        .samples_addr = samples,
        .count = KHR_PLOT_SAMPLE_COUNT,
        .amp = KHR_PLOT_AMP,
        .half_w = KHR_PLOT_HALF_W,
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
    khr_window_feed(topo, &client, &shell, &seat, nullptr, false, 0);
    if (client.display_error) {
        printf("  Present:      DISPLAY ERROR object=%u code=%u '%s'\n",
               client.error_object, client.error_code, client.error_msg);
        khr_cursor_destroy(&client, &cursor);
        khr_wl_client_disconnect(&client);
        return false;
    }

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
    float* samples = nullptr;
    VkDeviceAddress samples_addr = 0;
    if (!khr_bda_arena_alloc(arena, sizeof(float) * KHR_PLOT_SAMPLE_COUNT, 16,
                             (void**)&samples, &samples_addr)) {
        printf("  Present:      FAILED plot BDA alloc\n");
        khr_cursor_destroy(&client, &cursor);
        khr_wl_client_disconnect(&client);
        return false;
    }
    khr_plot_fill_demo_samples(samples, KHR_PLOT_SAMPLE_COUNT);
    khr_plot_push_t plot_push = {};
    khr_window_plot_push(&plot_push, samples_addr);

    khr_plot_pipeline_t plot = {};
    if (!khr_plot_pipeline_init(&plot, dev, VK_FORMAT_B8G8R8A8_UNORM)) {
        printf("  Present:      FAILED plot pipeline\n");
        khr_cursor_destroy(&client, &cursor);
        khr_wl_client_disconnect(&client);
        return false;
    }
    printf("  Plot:         %u samples BDA=0x%llx (demo series, no file ingest)\n",
           KHR_PLOT_SAMPLE_COUNT, (unsigned long long)samples_addr);

    khr_dmabuf_present_t dp = {};
    bool have_dp = false;
    bool ok = true;
    bool dirty = true;
    bool was_fullscreen = shell.fullscreen;
    uint32_t popup_shm_id = 0;
    khr_shm_pool_t popup_pool = { .fd = -1 };
    uint32_t popup_buffer_id = 0;
    uint32_t popup_parent_seq = 0;

    while (!shell.closed && !client.display_error && !khr_window_stop) {
        bool resizing =
            (shell.states & (1U << KHR_XDG_STATE_RESIZING)) != 0;
        /* A mapped grab popup must notice popup_done / parent clicks promptly.
         * Outside-app clicks only arrive as popup_done, and only if grab stuck. */
        uint32_t wait_ms = dirty ? (resizing ? 16U : 0U)
                                 : (shell.popup_live ? 16U : 500U);
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
        /* xdg_popup.grab only auto-dismisses clicks *outside our surfaces*.
         * Clicks on the parent, move/resize, and parent configure are ours:
         * destroy the popup (same rule GTK/Chromium apply, our loop). */
        bool parent_moved = shell.popup_live &&
                            (shell.size_seq != popup_parent_seq ||
                             want_w != buf_w || want_h != buf_h || resizing);
        if (shell.popup_done || parent_moved) {
            khr_window_popup_teardown(&client, &shell, &popup_pool,
                                      &popup_buffer_id);
        }
        bool on_popup = shell.popup_live &&
                        seat.pointer_surface == shell.popup_surface_id;
        if (seat.pointer_in) {
            uint32_t cserial = seat.enter_serial != 0 ? seat.enter_serial
                                                      : khr_seat_serial(&seat);
            khr_hit_t chit = on_popup ? KHR_HIT_POPUP : hit;
            (void)khr_cursor_apply(&client, &cursor, seat.pointer_id,
                                   cserial, chit, false);
        }
        if (seat.right_down) {
            if (on_popup) {
                /* Future: menu item. Keep the cart open. */
            } else if (hit == KHR_HIT_CLIENT) {
                khr_window_popup_teardown(&client, &shell, &popup_pool,
                                          &popup_buffer_id);
                uint32_t serial = seat.button_serial != 0 ? seat.button_serial
                                                          : khr_seat_serial(&seat);
                if (khr_xdg_popup_open(&client, &shell, seat.seat_id, serial,
                                       seat.x, seat.y,
                                       (int32_t)KHR_WINDOW_POPUP_W,
                                       (int32_t)KHR_WINDOW_POPUP_H)) {
                    popup_parent_seq = shell.size_seq;
                }
            } else if (shell.popup_live) {
                khr_window_popup_teardown(&client, &shell, &popup_pool,
                                          &popup_buffer_id);
            }
            seat.right_down = false;
        }
        if (seat.left_down && shell.popup_live && !on_popup) {
            khr_window_popup_teardown(&client, &shell, &popup_pool,
                                      &popup_buffer_id);
            seat.left_down = false;
        } else if (seat.double_click && hit == KHR_HIT_MOVE &&
                   !shell.fullscreen && !on_popup) {
            (void)khr_xdg_set_maximized(&client, &shell, !shell.maximized);
            seat.left_down = false;
        } else if (seat.left_down && on_popup) {
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
            if (shell.popup_live) {
                khr_window_popup_teardown(&client, &shell, &popup_pool,
                                          &popup_buffer_id);
            }
            (void)khr_xdg_set_fullscreen(&client, &shell, !shell.fullscreen);
        }
        if (seat.esc_pressed) {
            if (shell.popup_live) {
                khr_window_popup_teardown(&client, &shell, &popup_pool,
                                          &popup_buffer_id);
            } else if (shell.fullscreen) {
                (void)khr_xdg_set_fullscreen(&client, &shell, false);
            }
        }
        if (shell.popup_live && shell.popup_configured && !shell.popup_mapped) {
            uint32_t pw = (uint32_t)shell.popup_w;
            uint32_t ph = (uint32_t)shell.popup_h;
            if (pw == 0) {
                pw = KHR_WINDOW_POPUP_W;
            }
            if (ph == 0) {
                ph = KHR_WINDOW_POPUP_H;
            }
            if (popup_shm_id == 0) {
                (void)khr_shm_bind(&client, &popup_shm_id);
            }
            size_t bytes = (size_t)pw * (size_t)ph * 4U;
            if (popup_pool.addr != nullptr && popup_pool.size < bytes) {
                khr_shm_pool_destroy(&client, &popup_pool);
            }
            if (popup_pool.addr == nullptr && popup_shm_id != 0) {
                (void)khr_shm_pool_init(&client, popup_shm_id, bytes, &popup_pool);
            }
            if (popup_pool.addr != nullptr &&
                khr_shm_buffer_create(&client, &popup_pool, 0, pw, ph, pw * 4U,
                                      &popup_buffer_id)) {
                khr_window_paint_popup_shm((uint32_t*)popup_pool.addr, pw, ph);
                (void)khr_xdg_popup_ack(&client, &shell);
                (void)khr_shm_attach_commit(&client, shell.popup_surface_id,
                                            popup_buffer_id, pw, ph);
                shell.popup_mapped = true;
            }
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
            uint32_t top = shell.fullscreen ? 0U : KHR_WINDOW_CHROME_TOP;
            (void)khr_xdg_ack_pending(&client, &shell);
            if (khr_dmabuf_present_commit_scene(dev, &dp, cards_addr, 2,
                                                &plot, &plot_push, top)) {
                dirty = false;
                khr_window_wait_acquire(dev);
                (void)khr_dmabuf_present_sync(dev, &dp);
                if (dp.has_retiring) {
                    khr_gfx_device_wait_idle(dev);
                    khr_dmabuf_present_drop_retired(dev, &dp);
                }
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
    khr_window_popup_teardown(&client, &shell, &popup_pool, &popup_buffer_id);
    khr_plot_pipeline_destroy(&plot);
    khr_cursor_destroy(&client, &cursor);
    khr_wl_client_disconnect(&client);
    if (ok && !client.display_error) {
        ok = true;
    }
    return ok;
}

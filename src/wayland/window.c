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
#include "khoros/gfx/blob.h"
#include "khoros/uring/pbuf.h"
#include <vulkan/vulkan.h>

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
    float close_x = 0.0f;
    float close_y = 0.0f;
    float close_w = 0.0f;
    float close_h = 0.0f;
    if (!fullscreen && w >= KHR_WINDOW_CHROME_CLOSE) {
        close_x = (float)(w - KHR_WINDOW_CHROME_CLOSE) + 4.0f;
        close_y = 4.0f;
        close_w = (float)KHR_WINDOW_CHROME_CLOSE - 8.0f;
        close_h = (float)KHR_WINDOW_CHROME_TOP - 8.0f;
    }
    cards[2] = (khr_card_instance_t){
        .rect = { close_x, close_y, close_w, close_h },
        .bg_rgba = khr_rgba8(168, 56, 56, 255),
        .border_rgba = khr_rgba8(200, 88, 88, 255),
        .corner_radius = fullscreen ? 0.0f : 4.0f,
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

[[nodiscard]]
static int32_t khr_window_iabs(int32_t v) {
    return v < 0 ? -v : v;
}

[[nodiscard]]
static bool khr_window_mesh_from_payload(void* payload, size_t cap,
                                         VkDeviceAddress bda,
                                         khr_mesh_push_t* push, khr_cam_t* cam,
                                         bool reset_orient) {
    khr_blob_view_t v = {};
    if (!khr_mesh_bind_blob(payload, cap, bda, push) ||
        !khr_blob_parse(payload, cap, &v)) {
        return false;
    }
    khr_cam_frame(cam, v.verts, v.vert_count, reset_orient);
    khr_mesh_cam_apply(push, cam);
    return true;
}

static void khr_window_frame_payload(void* payload, size_t cap,
                                     khr_mesh_push_t* push, khr_cam_t* cam,
                                     bool reset_orient) {
    khr_blob_view_t v = {};
    if (!khr_blob_parse(payload, cap, &v)) {
        return;
    }
    khr_cam_frame(cam, v.verts, v.vert_count, reset_orient);
    khr_mesh_cam_apply(push, cam);
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

[[nodiscard]]
bool khr_window_run(khr_topology_t* topo, khr_gfx_device_t* dev,
                    khr_bda_arena_t* arena, const char* blob_path) {
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

    size_t hp_sz = arena->size;
    size_t ui_off = khr_hp_ui_off(hp_sz);
    size_t pay_cap = khr_hp_payload_cap(hp_sz);
    if (pay_cap == 0 || ui_off + sizeof(khr_card_instance_t) * KHR_WINDOW_CARD_COUNT > hp_sz ||
        arena->host_ptr == nullptr || arena->gpu_address == 0) {
        printf("  Present:      FAILED hugepage split\n");
        khr_cursor_destroy(&client, &cursor);
        khr_wl_client_disconnect(&client);
        return false;
    }
    khr_card_instance_t* cards =
        (khr_card_instance_t*)((uint8_t*)arena->host_ptr + ui_off);
    VkDeviceAddress cards_addr = arena->gpu_address + (VkDeviceAddress)ui_off;
    void* payload = arena->host_ptr;
    VkDeviceAddress payload_bda = arena->gpu_address;

    khr_mesh_push_t mesh_push = {};
    khr_cam_t cam = {};
    size_t box_n = khr_blob_write_box(payload, pay_cap);
    if (box_n == 0 ||
        !khr_window_mesh_from_payload(payload, pay_cap, payload_bda, &mesh_push,
                                      &cam, true)) {
        printf("  Present:      FAILED default box blob\n");
        khr_cursor_destroy(&client, &cursor);
        khr_wl_client_disconnect(&client);
        return false;
    }

    khr_mesh_push_t giz_bind = {};
    size_t giz_off = sizeof(khr_card_instance_t) * KHR_WINDOW_CARD_COUNT;
    uint8_t* giz_host = (uint8_t*)arena->host_ptr + ui_off + giz_off;
    size_t giz_cap = (giz_off < KHR_HP_UI_RESERVE)
                         ? (KHR_HP_UI_RESERVE - giz_off) : 0;
    size_t giz_n = khr_blob_write_gizmo_arm(giz_host, giz_cap);
    bool giz_ok = giz_n > 0 &&
                  khr_mesh_bind_blob(giz_host, giz_n,
                                     arena->gpu_address +
                                         (VkDeviceAddress)(ui_off + giz_off),
                                     &giz_bind);

    bool ingest_pending = false;
    bool imported = arena->is_imported && topo->hugepage == arena->host_ptr;
    if (blob_path != nullptr && blob_path[0] != '\0') {
        if (imported && khr_topology_ingest_submit(topo, blob_path)) {
            ingest_pending = true;
            printf("  Ingest:       submitted '%s' -> payload %zu B (async)\n",
                   blob_path, pay_cap);
        } else {
            printf("  Ingest:       skipped (%s) default box stays\n",
                   imported ? "submit failed" : "arena is not the hugepage");
        }
    }

    khr_mesh_pipeline_t mesh = {};
    if (!khr_mesh_pipeline_init(&mesh, dev, VK_FORMAT_B8G8R8A8_UNORM)) {
        printf("  Present:      FAILED mesh pipeline\n");
        khr_cursor_destroy(&client, &cursor);
        khr_wl_client_disconnect(&client);
        return false;
    }
    printf("  GPU:          %ux MSAA  depth=%s (cull back, GPU z-test)\n",
           (unsigned)khr_gfx_sample_count(dev),
           khr_gfx_depth_format(dev) == VK_FORMAT_D32_SFLOAT ? "D32"
           : (khr_gfx_depth_format(dev) != VK_FORMAT_UNDEFINED ? "D24" : "none"));
    printf("  Payload:      %zu KiB @0  UI %zu KiB @%zu  import=%s\n",
           pay_cap / 1024, KHR_HP_UI_RESERVE / 1024, ui_off,
           imported ? "yes" : "no");
    printf("  Mesh:         %u verts %u idx BDA=0x%llx (%s)\n",
           mesh_push.vert_count, mesh_push.index_count,
           (unsigned long long)mesh_push.verts_addr,
           ingest_pending ? "box until ingest" : "box");
    printf("  View:         LMB orbit  Shift/MMB pan  wheel zoom  F frame  gimbal snap\n");

    khr_dmabuf_present_t dp = {};
    bool have_dp = false;
    bool ok = true;
    bool dirty = true;
    bool was_fullscreen = shell.fullscreen;
    uint32_t popup_shm_id = 0;
    khr_shm_pool_t popup_pool = { .fd = -1 };
    uint32_t popup_buffer_id = 0;
    uint32_t popup_parent_w = 0;
    uint32_t popup_parent_h = 0;
    uint32_t cam_drag = 0; /* 1 = orbit, 2 = pan */
    int32_t drag_x = 0;
    int32_t drag_y = 0;
    int32_t drag_sx = 0;
    int32_t drag_sy = 0;
    khr_hit_t drag_hit = KHR_HIT_CLIENT;

    while (!shell.closed && !client.display_error && !khr_window_stop) {
        bool resizing =
            (shell.states & (1U << KHR_XDG_STATE_RESIZING)) != 0;
        /* A mapped grab popup must notice popup_done / parent clicks promptly.
         * Outside-app clicks only arrive as popup_done, and only if grab stuck.
         * Pointer-down view drags must not sit in a 500 ms wait. */
        bool view_drag = seat.left_down || seat.middle_down || cam_drag != 0;
        uint32_t wait_ms = dirty ? (resizing ? 16U : 0U)
                                 : (shell.popup_live || ingest_pending || view_drag
                                        ? 16U : 500U);
        khr_window_feed(topo, &client, &shell, &seat, have_dp ? &dp : nullptr,
                        have_dp, wait_ms);
        if (ingest_pending) {
            uint32_t was = topo->ingest_outstanding;
            size_t n = 0;
            bool got = khr_topology_pop_ingest(topo, &n);
            if (got || topo->ingest_outstanding != was) {
                ingest_pending = false;
                if (n == 0 ||
                    !khr_window_mesh_from_payload(payload, pay_cap, payload_bda,
                                                  &mesh_push, &cam, true)) {
                    (void)khr_blob_write_box(payload, pay_cap);
                    (void)khr_window_mesh_from_payload(payload, pay_cap,
                                                       payload_bda, &mesh_push,
                                                       &cam, true);
                    printf("  Ingest:       invalid blob, default box\n");
                } else {
                    printf("  Ingest:       %zu B  %u verts %u idx\n",
                           n, mesh_push.vert_count, mesh_push.index_count);
                }
                dirty = true;
            }
        }
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

        khr_hit_list_t hits = {};
        khr_window_hit_list_fill(&hits, buf_w, buf_h, shell.fullscreen);
        khr_hit_t hit = KHR_HIT_CLIENT;
        if (seat.x >= 0 && seat.y >= 0 &&
            (uint32_t)seat.x < buf_w && (uint32_t)seat.y < buf_h) {
            hit = khr_hit_list_pick(&hits, seat.x, seat.y);
        }
        /* Dismiss only when the parent buffer size actually changes.
         * The RESIZING state bit can stick after an edge drag; using it
         * here made every new cart die until a later move configure
         * cleared the bit. Clicks on the parent still teardown below. */
        bool parent_resized = shell.popup_live &&
                              (want_w != popup_parent_w ||
                               want_h != popup_parent_h);
        if (shell.popup_done || parent_resized) {
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
                    popup_parent_w = want_w;
                    popup_parent_h = want_h;
                }
            } else if (shell.popup_live) {
                khr_window_popup_teardown(&client, &shell, &popup_pool,
                                          &popup_buffer_id);
            }
            seat.right_down = false;
        }
        if (cam_drag == 0 && seat.left_down && shell.popup_live && !on_popup) {
            khr_window_popup_teardown(&client, &shell, &popup_pool,
                                      &popup_buffer_id);
            seat.left_down = false;
        } else if (cam_drag == 0 && seat.left_down && hit == KHR_HIT_CLOSE &&
                   !on_popup) {
            khr_window_stop = 1;
            seat.left_down = false;
        } else if (cam_drag == 0 && seat.double_click && hit == KHR_HIT_MOVE &&
                   !shell.fullscreen && !on_popup) {
            /* Same request as dragging the title bar to the top of the
             * output: xdg_toplevel.set_maximized. The compositor picks
             * the work-area size. Do not compute a pixel size here. */
            (void)khr_xdg_set_maximized(&client, &shell, !shell.maximized);
            seat.left_down = false;
            seat.double_click = false;
        } else if (cam_drag == 0 && seat.left_down && on_popup) {
            seat.left_down = false;
        } else if (cam_drag == 0 && seat.left_down && !on_popup &&
                   !shell.fullscreen) {
            uint32_t serial = khr_seat_serial(&seat);
            uint32_t edge = khr_hit_resize_edge(hit);
            if (hit == KHR_HIT_MOVE) {
                (void)khr_xdg_move(&client, &shell, seat.seat_id, serial);
                seat.left_down = false;
            } else if (edge != KHR_XDG_RESIZE_NONE) {
                (void)khr_xdg_resize(&client, &shell, seat.seat_id, serial, edge);
                seat.left_down = false;
            }
        }
        bool in_view = !on_popup &&
                       (hit == KHR_HIT_CLIENT || hit == KHR_HIT_GIMBAL);
        if (seat.double_click && in_view) {
            khr_window_frame_payload(payload, pay_cap, &mesh_push, &cam,
                                     hit == KHR_HIT_GIMBAL);
            dirty = true;
            seat.double_click = false;
            seat.left_down = false;
            cam_drag = 0;
        }
        if (seat.f_pressed && !on_popup) {
            khr_window_frame_payload(payload, pay_cap, &mesh_push, &cam, false);
            dirty = true;
            seat.f_pressed = false;
        }
        if (cam_drag == 0 && in_view) {
            if (seat.middle_down || (seat.left_down && seat.shift_down)) {
                cam_drag = 2;
                drag_sx = seat.x;
                drag_sy = seat.y;
                drag_x = seat.x;
                drag_y = seat.y;
                drag_hit = hit;
            } else if (seat.left_down) {
                cam_drag = 1;
                drag_sx = seat.x;
                drag_sy = seat.y;
                drag_x = seat.x;
                drag_y = seat.y;
                drag_hit = hit;
            }
        }
        if (cam_drag == 1 && seat.left_down) {
            int32_t dx = seat.x - drag_x;
            int32_t dy = seat.y - drag_y;
            if (dx != 0 || dy != 0) {
                khr_cam_orbit(&cam, (float)dx * 0.008f, (float)dy * 0.008f);
                khr_mesh_cam_apply(&mesh_push, &cam);
                dirty = true;
                drag_x = seat.x;
                drag_y = seat.y;
            }
        } else if (cam_drag == 2 &&
                   (seat.middle_down || (seat.left_down && seat.shift_down))) {
            int32_t dx = seat.x - drag_x;
            int32_t dy = seat.y - drag_y;
            if (dx != 0 || dy != 0) {
                khr_cam_pan(&cam, (float)dx, (float)dy);
                khr_mesh_cam_apply(&mesh_push, &cam);
                dirty = true;
                drag_x = seat.x;
                drag_y = seat.y;
            }
        }
        if (cam_drag == 1 && !seat.left_down) {
            int32_t tdx = khr_window_iabs(seat.x - drag_sx);
            int32_t tdy = khr_window_iabs(seat.y - drag_sy);
            if (drag_hit == KHR_HIT_GIMBAL &&
                tdx <= (int32_t)KHR_WINDOW_DBLCLICK_PX &&
                tdy <= (int32_t)KHR_WINDOW_DBLCLICK_PX) {
                uint32_t gx = 0, gy = 0, gs = 0;
                khr_window_gimbal_rect(buf_w, buf_h, shell.fullscreen,
                                       &gx, &gy, &gs);
                if (gs > 0) {
                    float cx = (float)gx + (float)gs * 0.5f;
                    float cy = (float)gy + (float)gs * 0.5f;
                    float nx = ((float)drag_sx - cx) / ((float)gs * 0.5f);
                    float ny = ((float)drag_sy - cy) / ((float)gs * 0.5f);
                    int axis = khr_cam_pick_axis(&cam, nx, ny);
                    if (axis != 0) {
                        khr_cam_snap_axis(&cam, axis);
                        khr_mesh_cam_apply(&mesh_push, &cam);
                        dirty = true;
                    }
                }
            }
            cam_drag = 0;
        }
        if (cam_drag == 2 &&
            !seat.middle_down && !(seat.left_down && seat.shift_down)) {
            cam_drag = 0;
        }
        if (seat.wheel != 0) {
            if ((in_view || cam_drag != 0) && !on_popup) {
                khr_cam_zoom(&cam, -(float)seat.wheel / 120.0f);
                khr_mesh_cam_apply(&mesh_push, &cam);
                dirty = true;
            }
            seat.wheel = 0;
        }
        if (seat.f11_pressed) {
            if (shell.popup_live) {
                khr_window_popup_teardown(&client, &shell, &popup_pool,
                                          &popup_buffer_id);
            }
            (void)khr_xdg_set_fullscreen(&client, &shell, !shell.fullscreen);
            seat.f11_pressed = false;
        }
        if (seat.esc_pressed) {
            if (shell.popup_live) {
                khr_window_popup_teardown(&client, &shell, &popup_pool,
                                          &popup_buffer_id);
            } else if (shell.fullscreen) {
                (void)khr_xdg_set_fullscreen(&client, &shell, false);
            }
            seat.esc_pressed = false;
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
            uint32_t gx = 0, gy = 0, gs = 0;
            khr_window_gimbal_rect(buf_w, buf_h, shell.fullscreen, &gx, &gy, &gs);
            khr_gizmo_pass_t giz = {};
            const khr_gizmo_pass_t* giz_arg = nullptr;
            if (giz_ok && gs > 0) {
                giz.verts_addr = giz_bind.verts_addr;
                giz.indices_addr = giz_bind.indices_addr;
                giz.index_count = giz_bind.index_count;
                giz.vert_count = giz_bind.vert_count;
                giz.x = gx;
                giz.y = gy;
                giz.s = gs;
                for (int i = 0; i < 3; i++) {
                    giz.r0[i] = cam.r0[i];
                    giz.r1[i] = cam.r1[i];
                    giz.r2[i] = cam.r2[i];
                }
                giz_arg = &giz;
            }
            (void)khr_xdg_ack_pending(&client, &shell);
            if (khr_dmabuf_present_commit_scene(dev, &dp, cards_addr,
                                                KHR_WINDOW_CARD_COUNT,
                                                nullptr, nullptr,
                                                &mesh, &mesh_push, giz_arg, top)) {
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
    khr_mesh_pipeline_destroy(&mesh);
    khr_cursor_destroy(&client, &cursor);
    khr_wl_client_disconnect(&client);
    if (ok && !client.display_error) {
        ok = true;
    }
    return ok;
}

#include "khoros/wayland/window.h"
#include "khoros/core/config.h"
#include "khoros/wayland/client.h"
#include "khoros/wayland/xdg.h"
#include "khoros/wayland/seat.h"
#include "khoros/wayland/cursor.h"
#include "khoros/wayland/dmabuf_present.h"
#include "khoros/wayland/shm.h"
#include "khoros/wayland/wire.h"
#include "khoros/wayland/presentation_time.h"
#include "khoros/gfx/pipeline.h"
#include "khoros/gfx/blob.h"
#include "khoros/gfx/cull_pipeline.h"
#include "khoros/gfx/hiz.h"
#include "khoros/gfx/scene.h"
#include "khoros/gfx/camera.h"
#include "khoros/core/input.h"
#include "khoros/audio/audio.h"
#include "khoros/uring/pbuf.h"
#include "khoros/gfx/descriptor_buffer.h"
#include "khoros/gfx/texture.h"
#include "khoros/gfx/texture_synth.h"
#include "khoros/gfx/grid.h"
#include "khoros/audio/synth.h"
#include "khoros/core/physics.h"
#include "khoros/core/deck.h"
#include "khoros/gfx/suzanne_data.h"
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

constexpr uint32_t KHR_HUM_FRAMES    = 48000;
constexpr uint32_t KHR_CLICK_FRAMES  = 2400;
constexpr uint32_t KHR_IMPACT_FRAMES = 21600;
constexpr uint32_t KHR_THUD_FRAMES   = 14400;
alignas(64) static float s_hum_samples[KHR_HUM_FRAMES];
alignas(64) static float s_click_samples[KHR_CLICK_FRAMES];
alignas(64) static float s_impact_samples[KHR_IMPACT_FRAMES];
alignas(64) static float s_thud_samples[KHR_THUD_FRAMES];

[[nodiscard]]
static uint32_t khr_rgba8(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    return r | (g << 8) | (b << 16) | (a << 24);
}

static void khr_window_feed(khr_topology_t* topo, khr_wl_client_t* client,
                            khr_xdg_shell_t* shell, khr_seat_t* seat,
                            khr_dmabuf_present_t* dp, bool have_dp,
                            khr_presentation_time_t* pt, bool have_pt,
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
            if (have_pt) {
                (void)khr_presentation_time_consume(pt, data, len);
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

static void khr_window_frame_payload(void* payload, size_t cap,
                                     khr_camera_t* camera,
                                     bool reset_orient) {
    khr_blob_view_t v = {};
    if (!khr_blob_parse(payload, cap, &v)) {
        return;
    }
    khr_camera_frame_verts(camera, v.verts, v.vert_count, reset_orient);
}

[[nodiscard]]
static bool khr_window_register_mesh_from_payload(void* payload, size_t cap,
                                                  VkDeviceAddress payload_bda,
                                                  khr_bda_arena_t* arena,
                                                  khr_scene_t* scene,
                                                  khr_camera_t* camera,
                                                  bool reset_orient,
                                                  uint32_t* out_vert_count,
                                                  uint32_t* out_index_count) {
    khr_blob_view_t v = {};
    if (!khr_blob_parse(payload, cap, &v)) {
        return false;
    }
    if (camera != nullptr) {
        khr_camera_frame_verts(camera, v.verts, v.vert_count, reset_orient);
    }
    void* n_host = nullptr;
    VkDeviceAddress n_gpu = 0;
    if (v.vert_count > 0 && arena != nullptr) {
        if (khr_bda_arena_alloc(arena, (size_t)v.vert_count * 3U * sizeof(float), 16, &n_host, &n_gpu)) {
            khr_blob_generate_smooth_normals(v.verts, v.vert_count, v.indices, v.index_count, (float*)n_host);
        }
    }
    if (scene != nullptr) {
        (void)khr_scene_register_mesh_normals(scene, 0,
                                              payload_bda + v.verts_byte_off,
                                              payload_bda + v.indices_byte_off,
                                              n_gpu,
                                              v.vert_count, v.index_count, 2.5f);
        if (scene->instance_count > 0) {
            scene->instances[0].radius = 2.5f;
            if (scene->instances_b != nullptr) {
                scene->instances_b[0].radius = 2.5f;
            }
        }
    }
    if (out_vert_count != nullptr) *out_vert_count = v.vert_count;
    if (out_index_count != nullptr) *out_index_count = v.index_count;
    return true;
}


[[nodiscard]]
bool khr_window_run(khr_topology_t* topo, khr_gfx_device_t* dev,
                    khr_bda_arena_t* arena, const char* blob_path,
                    const char* deck_path) {
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
        khr_window_feed(topo, &client, &shell, &seat, nullptr, false, nullptr,
                        false, 50);
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
    khr_window_feed(topo, &client, &shell, &seat, nullptr, false, nullptr,
                    false, 0);
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

    khr_camera_t camera = {};
    khr_camera_init(&camera, 1.04719755f, 1.0f, 0.1f);
    khr_camera_look_at(&camera, (float[]){ 0.0f, 0.0f, 3.2f }, (float[]){ 0.0f, 0.0f, 0.0f }, (float[]){ 0.0f, 1.0f, 0.0f });

    size_t box_n = 0;
    if (khr_suzanne_khrb_size > 0 && khr_suzanne_khrb_size <= pay_cap) {
        memcpy(payload, khr_suzanne_khrb_data, khr_suzanne_khrb_size);
        box_n = khr_suzanne_khrb_size;
    } else {
        box_n = khr_blob_write_box(payload, pay_cap);
    }
    if (box_n == 0) {
        printf("  Present:      FAILED default mesh blob\n");
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
        if (khr_topology_ingest_submit(topo, blob_path)) {
            ingest_pending = true;
            printf("  Ingest:       submitted '%s' -> payload %zu B (%s)\n",
                   blob_path, pay_cap, imported ? "zero-copy BDA" : "fallback bridge");
        } else {
            printf("  Ingest:       submit failed, default box stays\n");
        }
    }

    khr_mesh_pipeline_t mesh = {};
    if (!khr_mesh_pipeline_init(&mesh, dev, VK_FORMAT_B8G8R8A8_UNORM)) {
        printf("  Present:      FAILED mesh pipeline\n");
        khr_cursor_destroy(&client, &cursor);
        khr_wl_client_disconnect(&client);
        return false;
    }

    khr_cull_pipeline_t cull_pipe = {};
    if (!khr_cull_pipeline_init(&cull_pipe, dev)) {
        printf("  Present:      FAILED cull pipeline\n");
        khr_mesh_pipeline_destroy(&mesh);
        khr_cursor_destroy(&client, &cursor);
        khr_wl_client_disconnect(&client);
        return false;
    }

    /* Pillar 1 & 4: Unified Descriptor Heap & GPU-Driven Texture Synthesizer */
    khr_descriptor_heap_t heap = {};
    if (!khr_descriptor_heap_init(&heap, dev, 1024, 64)) {
        printf("  Present:      FAILED descriptor heap init\n");
        khr_cull_pipeline_destroy(&cull_pipe);
        khr_mesh_pipeline_destroy(&mesh);
        khr_cursor_destroy(&client, &cursor);
        khr_wl_client_disconnect(&client);
        return false;
    }

    /* Register default trilinear sampler on Set 1 */
    VkSamplerCreateInfo samp_ci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .maxLod = 16.0f,
    };
    VkSampler default_sampler = VK_NULL_HANDLE;
    vkCreateSampler(dev->device, &samp_ci, nullptr, &default_sampler);
    khr_descriptor_heap_register_sampler(&heap, default_sampler);

    /* Synthesize GPU Procedural Textures (0% CPU, 100% GPU Compute) */
    khr_texture_synth_pipeline_t tex_synth = {};
    khr_texture_t tex_damascus = {};
    khr_texture_t tex_normal = {};
    khr_texture_t tex_brushed = {};
    khr_texture_t tex_checker = {};
    khr_texture_t tex_marble = {};

    if (khr_texture_synth_pipeline_init(&tex_synth, dev)) {
        /* Texture 0: Damascus Steel / Woven Flow */
        float steel_tint[4] = { 0.92f, 0.94f, 0.98f, 1.0f };
        (void)khr_texture_synth_generate_2d(&tex_synth, &tex_damascus, 512, 512,
                                            KHR_TEX_SYNTH_DAMASCUS_STEEL, 12.0f, steel_tint);
        (void)khr_texture_register_heap(&tex_damascus, &heap);

        /* Texture 1: Tangent Normal Map */
        (void)khr_texture_synth_generate_2d(&tex_synth, &tex_normal, 512, 512,
                                            KHR_TEX_SYNTH_NORMAL_MAP, 14.0f, nullptr);
        (void)khr_texture_register_heap(&tex_normal, &heap);

        /* Texture 2: Brushed Bronze / Copper */
        float bronze_tint[4] = { 0.95f, 0.70f, 0.50f, 1.0f };
        (void)khr_texture_synth_generate_2d(&tex_synth, &tex_brushed, 512, 512,
                                            KHR_TEX_SYNTH_BRUSHED_METAL, 10.0f, bronze_tint);
        (void)khr_texture_register_heap(&tex_brushed, &heap);

        /* Texture 3: PBR Checkerboard */
        float checker_tint[4] = { 0.85f, 0.35f, 0.25f, 1.0f };
        (void)khr_texture_synth_generate_2d(&tex_synth, &tex_checker, 512, 512,
                                            KHR_TEX_SYNTH_PBR_CHECKER, 6.0f, checker_tint);
        (void)khr_texture_register_heap(&tex_checker, &heap);

        /* Texture 4: Veined Italian Carrara Marble */
        float marble_tint[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        (void)khr_texture_synth_generate_2d(&tex_synth, &tex_marble, 512, 512,
                                            KHR_TEX_SYNTH_MARBLE, 4.0f, marble_tint);
        (void)khr_texture_register_heap(&tex_marble, &heap);

        khr_texture_synth_pipeline_destroy(&tex_synth);
    }

    khr_mesh_instanced_pipeline_t inst_pipe = {};
    if (!khr_mesh_instanced_pipeline_init(&inst_pipe, dev, VK_FORMAT_B8G8R8A8_UNORM, &heap)) {
        printf("  Present:      FAILED mesh instanced pipeline\n");
        if (default_sampler != VK_NULL_HANDLE) vkDestroySampler(dev->device, default_sampler, nullptr);
        khr_descriptor_heap_destroy(&heap);
        khr_cull_pipeline_destroy(&cull_pipe);
        khr_mesh_pipeline_destroy(&mesh);
        khr_cursor_destroy(&client, &cursor);
        khr_wl_client_disconnect(&client);
        return false;
    }

    khr_bda_arena_t scene_arena = {};
    if (!khr_bda_arena_init(dev, &scene_arena, KHR_BDA_DEFAULT_ARENA_SZ)) {
        printf("  Present:      FAILED scene arena init\n");
        khr_mesh_instanced_pipeline_destroy(&inst_pipe);
        if (default_sampler != VK_NULL_HANDLE) vkDestroySampler(dev->device, default_sampler, nullptr);
        khr_descriptor_heap_destroy(&heap);
        khr_cull_pipeline_destroy(&cull_pipe);
        khr_mesh_pipeline_destroy(&mesh);
        khr_cursor_destroy(&client, &cursor);
        khr_wl_client_disconnect(&client);
        return false;
    }

    khr_scene_t scene = {};
    if (!khr_scene_init(&scene, dev, &scene_arena, 1024, 8)) {
        printf("  Present:      FAILED scene init\n");
        khr_bda_arena_destroy(dev, &scene_arena);
        khr_mesh_instanced_pipeline_destroy(&inst_pipe);
        if (default_sampler != VK_NULL_HANDLE) vkDestroySampler(dev->device, default_sampler, nullptr);
        khr_descriptor_heap_destroy(&heap);
        khr_cull_pipeline_destroy(&cull_pipe);
        khr_mesh_pipeline_destroy(&mesh);
        khr_cursor_destroy(&client, &cursor);
        khr_wl_client_disconnect(&client);
        return false;
    }

    khr_grid_pipeline_t grid_pipe = {};
    bool have_grid = khr_grid_pipeline_init(&grid_pipe, dev, VK_FORMAT_B8G8R8A8_UNORM, khr_gfx_depth_format(dev));

    /* Register Mesh 0 in Scene Graph with smooth normals */
    uint32_t mesh_vert_count = 0, mesh_index_count = 0;
    (void)khr_window_register_mesh_from_payload(payload, pay_cap, payload_bda,
                                               &scene_arena, &scene, &camera,
                                               true, &mesh_vert_count, &mesh_index_count);

    /* Register Procedural Meshes directly into BDA Scene Graph (Pillar 3) */
    VkDeviceAddress sph_v = 0, sph_n = 0, sph_i = 0;
    uint32_t sph_vc = 0, sph_ic = 0;
    (void)khr_scene_generate_sphere(&scene_arena, 1.0f, 24, 48, &sph_v, &sph_n, &sph_i, &sph_vc, &sph_ic);
    uint32_t sph_mesh_id = khr_scene_register_mesh(&scene, sph_v, sph_i, sph_n, sph_vc, sph_ic, 1.0f);

    VkDeviceAddress cyl_v = 0, cyl_n = 0, cyl_i = 0;
    uint32_t cyl_vc = 0, cyl_ic = 0;
    (void)khr_scene_generate_cylinder(&scene_arena, 1.2f, 1.5f, 36, &cyl_v, &cyl_n, &cyl_i, &cyl_vc, &cyl_ic);
    uint32_t cyl_mesh_id = khr_scene_register_mesh(&scene, cyl_v, cyl_i, cyl_n, cyl_vc, cyl_ic, 1.5f);

    VkDeviceAddress cube_v = 0, cube_n = 0, cube_i = 0;
    uint32_t cube_vc = 0, cube_ic = 0;
    (void)khr_scene_generate_chamfer_box(&scene_arena, 1.0f, 0.15f, &cube_v, &cube_n, &cube_i, &cube_vc, &cube_ic);
    uint32_t cube_mesh_id = khr_scene_register_mesh_normals(&scene, 3, cube_v, cube_i, cube_n, cube_vc, cube_ic, 1.73f);

    /* Check for experiment deck */
    khr_deck_t deck = {};
    bool have_deck = false;
    if (deck_path != nullptr && deck_path[0] != '\0') {
        have_deck = khr_deck_load_file(&deck, deck_path);
        if (have_deck) {
            printf("  Deck:         Loaded '%s' (%u bodies, dt=%.5f s, gravity=[%.2f, %.2f, %.2f])\n",
                   deck_path, deck.body_count, (double)deck.dt_s,
                   (double)deck.gravity[0], (double)deck.gravity[1], (double)deck.gravity[2]);
        }
    }

    uint32_t active_mesh_id = 0;

    if (have_deck && deck.body_count > 0) {
        bool any_sphere = false;
        bool any_box = false;
        for (uint32_t i = 0; i < deck.body_count; i++) {
            if (deck.bodies[i].shape.type == KHR_SHAPE_AABB) any_box = true;
            if (deck.bodies[i].shape.type == KHR_SHAPE_SPHERE) any_sphere = true;
        }
        if (any_sphere && !any_box) {
            active_mesh_id = sph_mesh_id;
        } else if (any_box && !any_sphere) {
            active_mesh_id = cube_mesh_id;
        } else if (any_sphere) {
            active_mesh_id = sph_mesh_id;
        } else {
            active_mesh_id = 0;
        }

        const float palette[][3] = {
            { 1.00f, 0.85f, 0.40f }, /* Damascus Gold */
            { 0.95f, 0.95f, 0.98f }, /* Chrome Silver */
            { 0.95f, 0.50f, 0.35f }, /* Brushed Copper */
            { 0.30f, 0.85f, 0.95f }, /* Cyan Crystal */
            { 0.95f, 0.30f, 0.45f }, /* Ruby Red */
            { 0.40f, 0.95f, 0.55f }, /* Emerald Green */
            { 0.70f, 0.50f, 0.95f }, /* Amethyst Purple */
            { 0.95f, 0.70f, 0.30f }, /* Amber Bronze */
        };
        const size_t pal_count = sizeof(palette) / sizeof(palette[0]);

        for (uint32_t i = 0; i < deck.body_count && i < 1024; i++) {
            const khr_rigid_body_t* b = &deck.bodies[i];
            if (b->shape.type == KHR_SHAPE_PLANE) continue;

            float r = 0.5f;
            float sx = 1.0f, sy = 1.0f, sz = 1.0f;
            uint32_t mid = active_mesh_id;
            if (b->shape.type == KHR_SHAPE_SPHERE) {
                r = b->shape.sphere.radius;
                sx = sy = sz = r;
                mid = sph_mesh_id;
            } else if (b->shape.type == KHR_SHAPE_AABB) {
                r = fmaxf(b->shape.aabb.half_extents[0], fmaxf(b->shape.aabb.half_extents[1], b->shape.aabb.half_extents[2]));
                sx = b->shape.aabb.half_extents[0];
                sy = b->shape.aabb.half_extents[1];
                sz = b->shape.aabb.half_extents[2];
                mid = cube_mesh_id;
            }

            const float* col = palette[i % pal_count];
            khr_gpu_instance_t inst = {
                .position = { b->position[0], b->position[1], b->position[2] },
                .radius = r,
                .rotation = { b->rotation[0], b->rotation[1], b->rotation[2], b->rotation[3] },
                .scale = { sx, sy, sz },
                .mesh_id = mid,
                .albedo = { col[0], col[1], col[2] },
                .roughness = 0.15f + ((float)(i % 5) * 0.08f),
                .metallic = 0.80f + ((float)(i % 3) * 0.08f),
                .ao = 1.0f,
                .albedo_tex_id = UINT32_MAX,
                .normal_tex_id = UINT32_MAX,
            };
            (void)khr_scene_add_instance(&scene, &inst);
        }

        if (deck.body_count > 4) {
            khr_camera_look_at(&camera, (float[]){ 0.0f, 6.0f, 18.0f },
                                        (float[]){ 0.0f, 1.0f, 0.0f },
                                        (float[]){ 0.0f, 1.0f, 0.0f });
        }
    } else {
        /* Instance 0: Cook-Torrance GGX PBR Metallic Damascus Suzanne (Hero Mesh) */
        khr_gpu_instance_t main_inst = {
            .position = { 0.0f, 0.4f, 0.0f },
            .radius = 2.5f,
            .rotation = { 0.0f, 0.0f, 0.0f, 1.0f },
            .scale = { 1.0f, 1.0f, 1.0f },
            .mesh_id = 0,
            .albedo = { 1.0f, 0.88f, 0.65f }, /* Damascened Gold/Steel */
            .roughness = 0.20f,
            .metallic = 0.95f,
            .ao = 1.0f,
            .albedo_tex_id = tex_damascus.descriptor_index,
            .normal_tex_id = tex_normal.descriptor_index,
        };
        (void)khr_scene_add_instance(&scene, &main_inst);

        /* Instance 1: Companion Left - Chrome Silver UV Sphere */
        khr_gpu_instance_t chrome_inst = {
            .position = { -3.5f, 0.0f, 0.0f },
            .radius = 1.5f,
            .rotation = { 0.0f, 0.0f, 0.0f, 1.0f },
            .scale = { 1.2f, 1.2f, 1.2f },
            .mesh_id = (sph_mesh_id != UINT32_MAX) ? sph_mesh_id : 0,
            .albedo = { 0.95f, 0.95f, 0.95f },
            .roughness = 0.08f,
            .metallic = 0.98f,
            .ao = 1.0f,
            .albedo_tex_id = UINT32_MAX,
            .normal_tex_id = UINT32_MAX,
        };
        (void)khr_scene_add_instance(&scene, &chrome_inst);

        /* Instance 2: Pedestal Column - Veined Italian Carrara Marble Column beneath Suzanne */
        khr_gpu_instance_t ped_inst = {
            .position = { 0.0f, -1.8f, 0.0f },
            .radius = 2.0f,
            .rotation = { 0.0f, 0.0f, 0.0f, 1.0f },
            .scale = { 1.6f, 0.8f, 1.6f },
            .mesh_id = (cyl_mesh_id != UINT32_MAX) ? cyl_mesh_id : 0,
            .albedo = { 0.95f, 0.95f, 0.98f },
            .roughness = 0.18f,
            .metallic = 0.05f,
            .ao = 1.0f,
            .albedo_tex_id = tex_marble.descriptor_index,
            .normal_tex_id = UINT32_MAX,
        };
        (void)khr_scene_add_instance(&scene, &ped_inst);

        /* Instance 3: Companion Right - Brushed Copper Cube */
        khr_gpu_instance_t copper_inst = {
            .position = { 3.5f, 0.0f, 0.0f },
            .radius = 1.5f,
            .rotation = { 0.0f, 0.0f, 0.0f, 1.0f },
            .scale = { 1.1f, 1.1f, 1.1f },
            .mesh_id = (cube_mesh_id != UINT32_MAX) ? cube_mesh_id : 0,
            .albedo = { 0.95f, 0.64f, 0.54f },
            .roughness = 0.30f,
            .metallic = 0.88f,
            .ao = 1.0f,
            .albedo_tex_id = tex_brushed.descriptor_index,
            .normal_tex_id = tex_normal.descriptor_index,
        };
        (void)khr_scene_add_instance(&scene, &copper_inst);
    }

    /* Real Physical Lights (Pillar D) */
    khr_gpu_light_t sun = {
        .type = KHR_LIGHT_DIRECTIONAL,
        .direction = { -0.577f, -0.577f, -0.577f },
        .color = { 1.0f, 0.98f, 0.95f },
        .intensity = 3.0f,
    };
    (void)khr_scene_add_light(&scene, &sun);

    khr_gpu_light_t point = {
        .type = KHR_LIGHT_POINT,
        .position = { 2.0f, 3.0f, 2.0f },
        .range = 12.0f,
        .color = { 1.0f, 0.6f, 0.2f },
        .intensity = 20.0f,
    };
    (void)khr_scene_add_light(&scene, &point);

    khr_gpu_light_t spot = {
        .type = KHR_LIGHT_SPOT,
        .position = { -2.5f, 3.5f, 3.0f },
        .direction = { 0.577f, -0.577f, -0.577f },
        .range = 15.0f,
        .color = { 0.3f, 0.7f, 1.0f },
        .intensity = 30.0f,
        .spot_inner = cosf(18.0f * (float)M_PI / 180.0f),
        .spot_outer = cosf(32.0f * (float)M_PI / 180.0f),
    };
    (void)khr_scene_add_light(&scene, &spot);

    if (have_deck) {
        (void)khr_topology_start_sim_deck(topo, 60, &deck,
                                          scene.instances, scene.instances_b,
                                          scene.instances_gpu, scene.instances_b_gpu);
        khr_topology_sim_set_motion(topo, true);
        printf("  Sim:          60 Hz fixed-tick LBVH physics for deck on Core 3 (%u bodies)\n", deck.body_count);
    } else if (scene.instance_count > 0) {
        (void)khr_topology_start_sim(topo, 60, scene.instance_count,
                                     scene.instances, scene.instances_b,
                                     scene.instances_gpu, scene.instances_b_gpu);
        khr_topology_sim_set_motion(topo, true);
        printf("  Sim:          60 Hz fixed-tick LBVH physics on Core 3 (double-buffered BDA Slot A/B)\n");
    }

    /* Streaming 3D Audio Engine over Direct Buffers (Pillar 6) */
    khr_audio_config_t acfg = {
        .sample_rate = 48000,
        .period_frames = 512,
        .alsa_card = 0,
        .alsa_device = 0,
        .custom_sink_fd = -1,
        .use_io_uring = false,
    };
    khr_audio_engine_t audio_engine = {};
    bool audio_live = khr_audio_engine_init(&audio_engine, &acfg);

    khr_audio_clip_t impact_clip = {};
    khr_audio_clip_t thud_clip = {};
    khr_audio_clip_t click_clip = {};
    khr_audio_clip_t hum_clip = {};

    (void)khr_audio_synth_impact(&impact_clip, s_impact_samples, KHR_IMPACT_FRAMES, 48000, 520.0f, 0.45f);
    (void)khr_audio_synth_thud(&thud_clip, s_thud_samples, KHR_THUD_FRAMES, 48000, 85.0f, 0.30f);
    (void)khr_audio_synth_click(&click_clip, s_click_samples, KHR_CLICK_FRAMES, 48000, 1800.0f, 0.05f);
    (void)khr_audio_synth_hum(&hum_clip, s_hum_samples, KHR_HUM_FRAMES, 48000, 100.0f, 1.0f);

    if (audio_live) {
        if (khr_audio_engine_start_worker(&audio_engine)) {
            printf("  Audio:        Procedural modal harmonic synthesis + 3D spatial mixer live (48 kHz direct PCM)\n");
        } else {
            audio_live = false;
        }
    }

    printf("  GPU:          %ux MSAA  depth=%s (cull back, GPU z-test)\n",
           (unsigned)khr_gfx_sample_count(dev),
           khr_gfx_depth_format(dev) == VK_FORMAT_D32_SFLOAT ? "D32"
           : (khr_gfx_depth_format(dev) != VK_FORMAT_UNDEFINED ? "D24" : "none"));
    printf("  Scene:        GPU-driven Hi-Z occlusion culling + Cook-Torrance GGX PBR + 3 physical lights\n");
    printf("  Payload:      %zu KiB @0  UI %zu KiB @%zu  import=%s\n",
           pay_cap / 1024, KHR_HP_UI_RESERVE / 1024, ui_off,
           imported ? "yes" : "no");
    printf("  Mesh:         %u verts %u idx BDA=0x%llx (%s)\n",
           mesh_vert_count, mesh_index_count,
           (unsigned long long)payload_bda,
           ingest_pending ? "box until ingest" : "box");
    printf("  View:         LMB orbit  Shift/MMB pan  wheel zoom  F frame  gimbal snap\n");

    khr_dmabuf_present_t dp = {};
    bool have_dp = false;
    khr_hiz_t hiz = {};
    bool have_hiz = false;
    khr_presentation_time_t pres_time = {};
    bool have_pt = khr_presentation_time_bind(&pres_time, &client);
    if (have_pt) {
        printf("  Pacing:       wp_presentation_time bound (hardware V-sync feedback enabled)\n");
    }

    /* Decoupled Action-Mapping Input System (Pillar 7) */
    khr_input_ring_t input_ring = {};
    khr_input_ring_init(&input_ring);
    khr_seat_attach_input_ring(&seat, &input_ring);

    khr_action_map_t action_map = {};
    khr_action_map_init(&action_map);
    constexpr uint32_t AXIS_LOOK_X = 0;
    constexpr uint32_t AXIS_LOOK_Y = 1;
    constexpr uint32_t AXIS_PAN_X  = 2;
    constexpr uint32_t AXIS_PAN_Y  = 3;
    constexpr uint32_t AXIS_ZOOM   = 4;
    constexpr uint32_t ACTION_FRAME   = 0;
    constexpr uint32_t ACTION_IMPULSE = 1;

    khr_action_map_bind_axis_mouse(&action_map, AXIS_LOOK_X, KHR_AXIS_SRC_MOUSE_DX, -0.008f);
    khr_action_map_bind_axis_mouse(&action_map, AXIS_LOOK_Y, KHR_AXIS_SRC_MOUSE_DY, 0.008f);
    khr_action_map_bind_axis_mouse(&action_map, AXIS_PAN_X,  KHR_AXIS_SRC_MOUSE_DX, 1.0f);
    khr_action_map_bind_axis_mouse(&action_map, AXIS_PAN_Y,  KHR_AXIS_SRC_MOUSE_DY, 1.0f);
    khr_action_map_bind_axis_mouse(&action_map, AXIS_ZOOM,   KHR_AXIS_SRC_MOUSE_WHEEL, -1.0f / 120.0f);
    khr_action_map_bind_digital(&action_map, ACTION_FRAME,   KHR_INPUT_KEY, 33); /* Key 'F' */
    khr_action_map_bind_digital(&action_map, ACTION_IMPULSE, KHR_INPUT_KEY, 57); /* Key Space */

    bool ok = true;
    bool dirty = true;
    bool was_fullscreen = shell.fullscreen;
    uint32_t popup_shm_id = 0;
    khr_shm_pool_t popup_pool = { .fd = -1 };
    uint32_t popup_buffer_id = 0;
    uint32_t popup_parent_w = 0;
    uint32_t popup_parent_h = 0;
    uint32_t cam_drag = 0; /* 1 = orbit, 2 = pan */
    int32_t drag_sx = 0;
    int32_t drag_sy = 0;
    khr_hit_t drag_hit = KHR_HIT_CLIENT;
    uint64_t last_commit_ns = 0;
    uint64_t min_interval_ns = 16'666'667ULL; /* 60 Hz default */

    while (!shell.closed && !client.display_error && !khr_window_stop) {
        struct timespec ts_now = {};
        clock_gettime(CLOCK_MONOTONIC, &ts_now);
        uint64_t now_ns = (uint64_t)ts_now.tv_sec * 1'000'000'000ULL + (uint64_t)ts_now.tv_nsec;

        if (have_pt && pres_time.refresh_ns >= 4'000'000U && pres_time.refresh_ns <= 50'000'000U) {
            if (pres_time.refresh_ns <= 10'000'000U) {
                min_interval_ns = (uint64_t)pres_time.refresh_ns * 2ULL; /* Pace 120/144 Hz display at 60 Hz */
            } else {
                min_interval_ns = (uint64_t)pres_time.refresh_ns;
            }
            if (min_interval_ns < 16'666'667ULL) {
                min_interval_ns = 16'666'667ULL;
            }
        }

        bool resizing =
            (shell.states & (1U << KHR_XDG_STATE_RESIZING)) != 0;
        bool view_drag = seat.left_down || seat.middle_down || cam_drag != 0;
        bool sim_moving = topo->sim.active && khr_topology_sim_has_motion(topo);
        bool slots_full = have_dp && (khr_dmabuf_present_next_free(&dp) == UINT32_MAX);

        bool time_ok = (last_commit_ns == 0) || (now_ns >= last_commit_ns + min_interval_ns) || (now_ns < last_commit_ns);
        uint64_t next_deadline_ns = last_commit_ns + min_interval_ns;
        uint32_t frame_remain_ms = (next_deadline_ns > now_ns)
            ? (uint32_t)((next_deadline_ns - now_ns + 999'999ULL) / 1'000'000ULL)
            : 0U;
        uint32_t interval_ms = (uint32_t)(min_interval_ns / 1'000'000ULL);
        if (interval_ms == 0) interval_ms = 8U;

        uint32_t wait_ms = 500U;
        if (dirty) {
            if (!time_ok) {
                wait_ms = (frame_remain_ms > 0) ? frame_remain_ms : 1U;
            } else if (slots_full) {
                wait_ms = interval_ms;
            } else {
                wait_ms = 0U;
            }
        } else if (resizing || view_drag || shell.popup_live || ingest_pending || sim_moving) {
            wait_ms = interval_ms;
        } else {
            wait_ms = 500U;
        }

        khr_window_feed(topo, &client, &shell, &seat, have_dp ? &dp : nullptr,
                        have_dp, have_pt ? &pres_time : nullptr, have_pt, wait_ms);
        if (!seat.pointer_in) {
            cam_drag = 0;
        }
        khr_action_map_tick(&action_map, &input_ring, 1.0f / 60.0f);
        if (audio_live) {
            float fwd[3] = {
                camera.target[0] - camera.eye[0],
                camera.target[1] - camera.eye[1],
                camera.target[2] - camera.eye[2],
            };
            float f_len = sqrtf(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
            if (f_len > 1.0e-5f) {
                fwd[0] /= f_len; fwd[1] /= f_len; fwd[2] /= f_len;
            } else {
                fwd[0] = 0.0f; fwd[1] = 0.0f; fwd[2] = -1.0f;
            }
            float rx = fwd[1] * camera.up[2] - fwd[2] * camera.up[1];
            float ry = fwd[2] * camera.up[0] - fwd[0] * camera.up[2];
            float rz = fwd[0] * camera.up[1] - fwd[1] * camera.up[0];
            float r_len = sqrtf(rx * rx + ry * ry + rz * rz);
            if (r_len > 1.0e-5f) {
                rx /= r_len; ry /= r_len; rz /= r_len;
            } else {
                rx = 1.0f; ry = 0.0f; rz = 0.0f;
            }
            khr_audio_listener_t listener = {
                .px = camera.eye[0],
                .py = camera.eye[1],
                .pz = camera.eye[2],
                .vx = 0.0f, .vy = 0.0f, .vz = 0.0f,
                .fx = fwd[0], .fy = fwd[1], .fz = fwd[2],
                .ux = camera.up[0], .uy = camera.up[1], .uz = camera.up[2],
                .rx = rx, .ry = ry, .rz = rz,
            };
            khr_audio_engine_set_listener(&audio_engine, &listener);
        }
        if (topo->sim.active) {
            uint64_t sim_t = 0;
            if (khr_topology_pop_tick(topo, &sim_t)) {
                if (khr_topology_sim_has_motion(topo)) {
                    dirty = true;
                }
            }
        }
        if (ingest_pending) {
            uint32_t was = topo->ingest_outstanding;
            size_t n = 0;
            bool got = khr_topology_pop_ingest(topo, &n);
            if (got || topo->ingest_outstanding != was) {
                ingest_pending = false;
                bool valid = (n > 0 && n <= pay_cap && (n % sizeof(uint32_t) == 0));
                if (valid && !imported) {
                    memcpy(payload, topo->hugepage, n);
                }
                if (!valid ||
                    !khr_window_register_mesh_from_payload(payload, pay_cap, payload_bda,
                                                           &scene_arena, &scene, &camera,
                                                           true, &mesh_vert_count, &mesh_index_count)) {
                    (void)khr_blob_write_box(payload, pay_cap);
                    (void)khr_window_register_mesh_from_payload(payload, pay_cap, payload_bda,
                                                               &scene_arena, &scene, &camera,
                                                               true, &mesh_vert_count, &mesh_index_count);
                    printf("  Ingest:       invalid blob, default box\n");
                } else {
                    printf("  Ingest:       %zu B  %u verts %u idx (smooth normals generated)\n",
                           n, mesh_vert_count, mesh_index_count);
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
            uint32_t init_top = shell.fullscreen ? 0U : KHR_WINDOW_CHROME_TOP;
            uint32_t init_ph = (buf_h > init_top) ? (buf_h - init_top) : 1U;
            khr_camera_set_aspect(&camera, (float)buf_w / (float)init_ph);
            (void)khr_xdg_set_window_geometry(&client, &shell, 0, 0,
                                              (int32_t)want_w, (int32_t)want_h);
            have_hiz = khr_hiz_init(&hiz, dev, buf_w, buf_h);
            if (have_hiz) {
                printf("  Hi-Z:         %ux%u pyramid (%u mips, 2-pass occlusion culling enabled)\n",
                       buf_w, buf_h, hiz.mip_levels);
            }
            printf("  Present:      slots %ux%u\n", buf_w, buf_h);
        } else if (want_w != dp.width || want_h != dp.height) {
            dirty = true;
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
        bool in_view = !on_popup && (hit == KHR_HIT_CLIENT || hit == KHR_HIT_GIMBAL);
        bool frame_req = (seat.f_pressed || khr_action_map_just_pressed(&action_map, ACTION_FRAME)) && !on_popup;
        if (seat.double_click && in_view) {
            khr_window_frame_payload(payload, pay_cap, &camera, hit == KHR_HIT_GIMBAL);
            dirty = true;
            seat.double_click = false;
            seat.left_down = false;
            cam_drag = 0;
            if (audio_live) {
                (void)khr_audio_engine_play(&audio_engine, &click_clip, camera.eye[0], camera.eye[1], camera.eye[2], 0.7f, false);
            }
        } else if (frame_req) {
            khr_window_frame_payload(payload, pay_cap, &camera, false);
            dirty = true;
            seat.f_pressed = false;
            if (audio_live) {
                (void)khr_audio_engine_play(&audio_engine, &click_clip, camera.eye[0], camera.eye[1], camera.eye[2], 0.7f, false);
            }
        }
        /*
         * ARCHITECTURE NOTE (Version 0.2 Pose Ownership):
         * Camera strictly writes view transforms (eye, target, up).
         * Physical simulation on Core 3 owns dynamic body poses in double-buffered BDA Slot A/B.
         * Interactive impulses are delivered via khr_topology_sim_apply_impulse with SI units (N*s, m).
         */
        bool impulse_req = khr_action_map_just_pressed(&action_map, ACTION_IMPULSE) && !on_popup;
        if (impulse_req) {
            khr_topology_sim_set_motion(topo, true);
            dirty = true;
            for (uint32_t i = 0; i < scene.instance_count; i++) {
                float kick[3] = {
                    ((float)(i % 3) - 1.0f) * 3.0f,
                    10.0f + (float)i * 1.5f,
                    ((float)((i + 1) % 3) - 1.0f) * 3.0f
                };
                float r_pos[3] = { 0.2f * (float)(i + 1), 0.1f, 0.1f };
                khr_topology_sim_apply_impulse(topo, i, kick, r_pos);
            }
            if (audio_live) {
                (void)khr_audio_engine_play(&audio_engine, &click_clip, camera.eye[0], camera.eye[1], camera.eye[2], 1.0f, false);
            }
        }
        if (cam_drag == 0 && in_view) {
            if (seat.middle_down || (seat.left_down && seat.shift_down)) {
                cam_drag = 2;
                drag_sx = seat.x;
                drag_sy = seat.y;
                drag_hit = hit;
            } else if (seat.left_down) {
                cam_drag = 1;
                drag_sx = seat.x;
                drag_sy = seat.y;
                drag_hit = hit;
            }
        }
        if (cam_drag == 1 && seat.left_down) {
            float look_dx = khr_action_map_sample_axis(&action_map, AXIS_LOOK_X, 1.0f);
            float look_dy = khr_action_map_sample_axis(&action_map, AXIS_LOOK_Y, 1.0f);
            if (look_dx != 0.0f || look_dy != 0.0f) {
                khr_camera_orbit(&camera, look_dx, look_dy);
                dirty = true;
            }
        } else if (cam_drag == 2 && (seat.middle_down || (seat.left_down && seat.shift_down))) {
            float pan_dx = khr_action_map_sample_axis(&action_map, AXIS_PAN_X, 1.0f);
            float pan_dy = khr_action_map_sample_axis(&action_map, AXIS_PAN_Y, 1.0f);
            if (pan_dx != 0.0f || pan_dy != 0.0f) {
                khr_camera_pan(&camera, pan_dx, pan_dy);
                dirty = true;
            }
        }
        if (cam_drag == 1 && !seat.left_down) {
            int32_t tdx = khr_window_iabs(seat.x - drag_sx);
            int32_t tdy = khr_window_iabs(seat.y - drag_sy);
            if (drag_hit == KHR_HIT_GIMBAL &&
                tdx <= (int32_t)KHR_WINDOW_DBLCLICK_PX &&
                tdy <= (int32_t)KHR_WINDOW_DBLCLICK_PX) {
                uint32_t gx = 0, gy = 0, gs = 0;
                khr_window_gimbal_rect(buf_w, buf_h, shell.fullscreen, &gx, &gy, &gs);
                if (gs > 0) {
                    float cx = (float)gx + (float)gs * 0.5f;
                    float cy = (float)gy + (float)gs * 0.5f;
                    float nx = ((float)drag_sx - cx) / ((float)gs * 0.5f);
                    float ny = ((float)drag_sy - cy) / ((float)gs * 0.5f);
                    int axis = khr_camera_pick_axis(&camera, nx, ny);
                    if (axis != 0) {
                        khr_camera_snap_axis(&camera, axis);
                        dirty = true;
                        if (audio_live) {
                            (void)khr_audio_engine_play(&audio_engine, &click_clip, camera.eye[0], camera.eye[1], camera.eye[2], 0.7f, false);
                        }
                    }
                }
            }
            cam_drag = 0;
        }
        if (cam_drag == 2 && !seat.middle_down && !(seat.left_down && seat.shift_down)) {
            cam_drag = 0;
        }
        float zoom = khr_action_map_sample_axis(&action_map, AXIS_ZOOM, 1.0f);
        if (zoom != 0.0f) {
            if ((in_view || cam_drag != 0) && !on_popup) {
                khr_camera_zoom(&camera, zoom);
                dirty = true;
            }
        }
        seat.wheel = 0;
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
            if (dp.has_retiring && khr_dmabuf_present_can_drop_retired(dev, &dp)) {
                khr_dmabuf_present_drop_retired(dev, &dp);
            }
        }

        clock_gettime(CLOCK_MONOTONIC, &ts_now);
        now_ns = (uint64_t)ts_now.tv_sec * 1'000'000'000ULL + (uint64_t)ts_now.tv_nsec;
        time_ok = (last_commit_ns == 0) || (now_ns >= last_commit_ns + min_interval_ns) || (now_ns < last_commit_ns);

        bool need_resize = have_dp && (dp.width != want_w || dp.height != want_h);
        bool can_render = have_dp && dirty && time_ok &&
                          (need_resize || khr_dmabuf_present_next_free(&dp) != UINT32_MAX);
        if (can_render) {
            if (need_resize) {
                if (!khr_dmabuf_present_resize(dev, &dp, want_w, want_h)) {
                    printf("  Present:      FAILED resize %ux%u\n", want_w, want_h);
                    continue;
                }
                buf_w = want_w;
                buf_h = want_h;
                uint32_t resize_top = shell.fullscreen ? 0U : KHR_WINDOW_CHROME_TOP;
                uint32_t resize_ph = (buf_h > resize_top) ? (buf_h - resize_top) : 1U;
                khr_camera_set_aspect(&camera, (float)buf_w / (float)resize_ph);
                (void)khr_xdg_set_window_geometry(&client, &shell, 0, 0,
                                                  (int32_t)want_w, (int32_t)want_h);
                if (have_hiz) {
                    khr_hiz_destroy(&hiz);
                    have_hiz = khr_hiz_init(&hiz, dev, buf_w, buf_h);
                }
            }

            uint32_t top = shell.fullscreen ? 0U : KHR_WINDOW_CHROME_TOP;
            uint32_t ph = (buf_h > top) ? (buf_h - top) : 1U;
            float want_aspect = (float)buf_w / (float)ph;
            if (fabsf(camera.aspect - want_aspect) > 1.0e-4f) {
                khr_camera_set_aspect(&camera, want_aspect);
            }
            khr_window_paint_cards(cards, buf_w, buf_h, shell.fullscreen);
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
                khr_camera_get_rotation_matrix(&camera, giz.r0, giz.r1, giz.r2);
                giz_arg = &giz;
            }
            (void)khr_xdg_ack_pending(&client, &shell);

            khr_cull_push_t cull_push = {};
            khr_scene_prepare_cull_push(&scene, &camera, &cull_push);

            khr_hiz_cull_push_t hiz_push = {};
            if (have_hiz) {
                khr_scene_prepare_hiz_cull_push(&scene, &camera, &hiz, &hiz_push);
            }

            if (topo->sim.active && khr_topology_sim_has_motion(topo)) {
                uint64_t read_bda = 0;
                uint64_t prev_bda = 0;
                float alpha = 0.0f;
                uint32_t flags = 0;
                khr_topology_sim_get_render_state(topo, now_ns, &read_bda, &prev_bda, &alpha, &flags);
                if (read_bda != 0) {
                    cull_push.instances_addr = read_bda;
                    cull_push.alpha = alpha;
                    cull_push.flags = flags;
                    if (have_hiz) {
                        hiz_push.instances_addr = read_bda;
                        hiz_push.alpha = alpha;
                        hiz_push.flags |= flags;
                    }
                }
            } else {
                cull_push.instances_addr = scene.instances_gpu;
                cull_push.alpha = 0.0f;
                cull_push.flags = 0;
            }

            khr_mesh_instanced_push_t inst_push = {};
            khr_scene_prepare_mesh_push(&scene, active_mesh_id, &camera, &inst_push);

            float sim_floor = have_deck ? deck.floor_y : KHR_PHYSICS_FLOOR_Y;
            khr_grid_push_t grid_push = {
                .eye_plane_y = { camera.eye[0], camera.eye[1], camera.eye[2], sim_floor },
                .target_minor_sz = { camera.target[0], camera.target[1], camera.target[2], 1.0f },
                .up_major_sz = { camera.up[0], camera.up[1], camera.up[2], 5.0f },
                .params = { camera.fov_y, camera.aspect, camera.z_near, 120.0f },
            };

            khr_gpu_scene_pass_t gpu_pass = {
                .cull_pipe = &cull_pipe,
                .cull_push = &cull_push,
                .inst_pipe = &inst_pipe,
                .inst_push = &inst_push,
                .indirect_cmd_buffer = scene_arena.buffer,
                .indirect_cmd_offset = (VkDeviceSize)(scene.draw_cmd_gpu - scene_arena.gpu_address),
                .draw_count = 1,
                .hiz = have_hiz ? &hiz : nullptr,
                .hiz_push = have_hiz ? &hiz_push : nullptr,
                .descriptor_heap = &heap,
                .grid_pipe = have_grid ? &grid_pipe : nullptr,
                .grid_push = have_grid ? &grid_push : nullptr,
            };

            if (khr_dmabuf_present_commit_scene(dev, &dp, cards_addr,
                                                KHR_WINDOW_CARD_COUNT,
                                                nullptr, nullptr,
                                                &mesh, nullptr,
                                                &gpu_pass,
                                                giz_arg, top)) {
                dirty = false;
                last_commit_ns = now_ns;
                (void)khr_dmabuf_present_sync(dev, &dp);
                if (have_pt) {
                    uint32_t fid = 0;
                    (void)khr_presentation_request_feedback(&pres_time, shell.surface_id, dp.frames, &fid);
                }
                if (dp.has_retiring && khr_dmabuf_present_can_drop_retired(dev, &dp)) {
                    khr_dmabuf_present_drop_retired(dev, &dp);
                }
            }

            /* Harvest physical collision audio events from Core 3 simulation */
            khr_collision_event_t col_evt = {};
            while (khr_topology_sim_pop_collision_event(topo, &col_evt)) {
                if (audio_live) {
                    const khr_audio_clip_t* clip = &impact_clip;
                    if (col_evt.sound_type == KHR_COLLISION_SOUND_THUD) {
                        clip = &thud_clip;
                    } else if (col_evt.sound_type == KHR_COLLISION_SOUND_CLICK) {
                        clip = &click_clip;
                    }
                    float gain = fminf(0.8f, fmaxf(0.15f, col_evt.impulse * 0.25f));
                    (void)khr_audio_engine_play(&audio_engine, clip,
                                                col_evt.point[0], col_evt.point[1], col_evt.point[2],
                                                gain, false);
                }
            }

            if (audio_live) {
                float fwd[3] = {
                    camera.target[0] - camera.eye[0],
                    camera.target[1] - camera.eye[1],
                    camera.target[2] - camera.eye[2],
                };
                float flen = sqrtf(fwd[0]*fwd[0] + fwd[1]*fwd[1] + fwd[2]*fwd[2]);
                if (flen > 1e-4f) {
                    fwd[0] /= flen; fwd[1] /= flen; fwd[2] /= flen;
                }
                float rgt[3];
                khr_vec3_cross(rgt, fwd, camera.up);
                khr_audio_listener_t listener = {
                    .px = camera.eye[0], .py = camera.eye[1], .pz = camera.eye[2],
                    .vx = 0.0f, .vy = 0.0f, .vz = 0.0f,
                    .fx = fwd[0], .fy = fwd[1], .fz = fwd[2],
                    .ux = camera.up[0], .uy = camera.up[1], .uz = camera.up[2],
                    .rx = rgt[0], .ry = rgt[1], .rz = rgt[2],
                };
                khr_audio_engine_set_listener(&audio_engine, &listener);
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
    if (have_pt) {
        khr_presentation_time_destroy(&pres_time);
    }
    if (have_hiz) {
        khr_hiz_destroy(&hiz);
    }
    if (audio_live) {
        khr_audio_engine_stop_worker(&audio_engine);
        khr_audio_engine_destroy(&audio_engine);
    }
    if (topo->sim.active) {
        khr_topology_stop_sim(topo);
    }
    khr_scene_destroy(&scene);
    khr_bda_arena_destroy(dev, &scene_arena);
    khr_mesh_instanced_pipeline_destroy(&inst_pipe);
    if (have_grid) {
        khr_grid_pipeline_destroy(&grid_pipe);
    }
    khr_texture_destroy(&tex_marble);
    khr_texture_destroy(&tex_checker);
    khr_texture_destroy(&tex_brushed);
    khr_texture_destroy(&tex_normal);
    khr_texture_destroy(&tex_damascus);
    if (default_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(dev->device, default_sampler, nullptr);
    }
    khr_descriptor_heap_destroy(&heap);
    khr_cull_pipeline_destroy(&cull_pipe);
    khr_mesh_pipeline_destroy(&mesh);
    khr_cursor_destroy(&client, &cursor);
    khr_wl_client_disconnect(&client);
    if (ok && !client.display_error) {
        ok = true;
    }
    return ok;
}

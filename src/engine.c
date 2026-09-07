#include "engine.h"
#include "khoros/core/topology.h"
#include "khoros/uring/pbuf.h"
#include "khoros/wayland/client.h"
#include "khoros/wayland/xdg.h"
#include "khoros/wayland/dmabuf_present.h"
#include "khoros/wayland/wire.h"
#include "khoros/gfx/device.h"
#include "khoros/gfx/bda_arena.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

constexpr uint32_t KHR_ENGINE_PRESENT_W      = 320;
constexpr uint32_t KHR_ENGINE_PRESENT_H      = 240;
constexpr uint32_t KHR_ENGINE_PRESENT_FRAMES = 24;
constexpr uint32_t KHR_ENGINE_PRESENT_MS     = 8'000;

[[nodiscard]]
const char* engine_get_banner(void) {
    static const char banner[] = "=== Khoros Engine (Linux 7.2 / C23 / io_uring / Vulkan 1.4) ===";
    return banner;
}

[[nodiscard]]
static uint64_t khr_engine_now_ms(void) {
    struct timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1'000'000ULL;
}

static void khr_engine_feed(khr_topology_t* topo, khr_wl_client_t* client,
                            khr_xdg_shell_t* shell, khr_dmabuf_present_t* dp,
                            bool have_dp, uint32_t wait_ms) {
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
        uint32_t n = khr_wl_client_feed_cqe(client, evt.user_data, evt.res,
                                            evt.flags, &data, &len, &need_rearm);
        (void)n;
        if (need_rearm) {
            rearm = true;
        }
        if (data != nullptr && len >= 8) {
            (void)khr_xdg_consume(client, shell, data, len);
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

[[nodiscard]]
static bool khr_engine_present(khr_topology_t* topo, khr_gfx_device_t* dev,
                               khr_bda_arena_t* arena) {
    khr_wl_client_t client = {};
    if (!khr_wl_client_connect(&client, &topo->ring_a, nullptr)) {
        printf("  Present:      skipped (no compositor socket)\n");
        return true;
    }
    khr_wl_client_attach_pbufs(&client, &topo->pbuf_tier0, &topo->pbuf_tier1);
    khr_wl_client_attach_topology(&client, topo);
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
    printf("  Registry:     %u globals (compositor=%s xdg=%s dmabuf=%s syncobj=%s)\n",
           client.globals_count,
           client.compositor_name ? "yes" : "no",
           client.xdg_wm_base_name ? "yes" : "no",
           client.dmabuf_name ? "yes" : "no",
           client.syncobj_manager_name ? "yes" : "no");
    if (!have_globals) {
        printf("  Present:      skipped (missing required globals)\n");
        khr_wl_client_disconnect(&client);
        return true;
    }

    khr_xdg_shell_t shell = {};
    khr_xdg_init(&shell);
    if (!khr_xdg_bind(&client, &shell) ||
        !khr_xdg_create_toplevel(&client, &shell, "Khoros", "khoros-engine") ||
        !khr_wl_client_arm_inbound(&client)) {
        printf("  Present:      FAILED xdg setup\n");
        khr_wl_client_disconnect(&client);
        return false;
    }

    uint64_t deadline = khr_engine_now_ms() + 5'000;
    while (!shell.configured && !shell.closed && !client.display_error &&
           khr_engine_now_ms() < deadline) {
        khr_engine_feed(topo, &client, &shell, nullptr, false, 200);
    }
    if (client.display_error) {
        printf("  Present:      DISPLAY ERROR object=%u code=%u '%s'\n",
               client.error_object, client.error_code, client.error_msg);
        khr_wl_client_disconnect(&client);
        return false;
    }
    if (!khr_xdg_can_attach(&shell)) {
        printf("  Present:      FAILED attach gate (configured=%d closed=%d)\n",
               shell.configured, shell.closed);
        khr_wl_client_disconnect(&client);
        return false;
    }
    printf("  XDG:          configured %dx%d serial=%u\n",
           shell.width, shell.height, shell.last_ack_serial);

    khr_dmabuf_present_t dp = {};
    if (!khr_dmabuf_present_init(dev, &client, shell.surface_id,
                                 KHR_ENGINE_PRESENT_W, KHR_ENGINE_PRESENT_H,
                                 &dp)) {
        printf("  Present:      FAILED dmabuf present init\n");
        khr_wl_client_disconnect(&client);
        return false;
    }

    deadline = khr_engine_now_ms() + KHR_ENGINE_PRESENT_MS;
    while (khr_engine_now_ms() < deadline && !shell.closed &&
           !client.display_error && dp.frames < KHR_ENGINE_PRESENT_FRAMES) {
        khr_engine_feed(topo, &client, &shell, &dp, true, 8);
        (void)khr_dmabuf_present_sync(dev, &dp);
        if (khr_dmabuf_present_next_free(&dp) != UINT32_MAX) {
            khr_bda_arena_reset(arena);
            (void)khr_dmabuf_present_commit_frame(dev, &dp, arena, dp.frames + 1);
        }
    }
    khr_engine_feed(topo, &client, &shell, &dp, true, 50);

    bool alive = false;
    if (!shell.closed && !client.display_error) {
        uint32_t cb = khr_wl_client_alloc_id(&client);
        khr_wl_msg_buf_t sync = {};
        khr_wl_buf_init(&sync);
        if (khr_wl_encode_header(&sync, KHR_WL_DISPLAY_ID, KHR_WL_DISPLAY_SYNC, 12) &&
            khr_wl_encode_u32(&sync, cb) &&
            khr_wl_client_send_skip(&client, sync.data, sync.size)) {
            uint64_t dl = khr_engine_now_ms() + 1'000;
            while (khr_engine_now_ms() < dl && !shell.closed &&
                   !client.display_error) {
                khr_engine_feed(topo, &client, &shell, &dp, true, 50);
            }
            alive = !shell.closed && !client.display_error;
        }
    }

    printf("  Present:      frames=%llu releases=%u created=%u failed=%u "
           "pushed=%llu alive=%s%s%s\n",
           (unsigned long long)dp.frames, dp.releases, dp.created_count,
           dp.failed_count, (unsigned long long)dp.last_signaled,
           alive ? "yes" : "no",
           client.display_error ? " DISPLAY_ERROR" : "",
           shell.closed ? " closed" : "");
    if (client.display_error) {
        printf("  Error:        object=%u code=%u '%s'\n",
               client.error_object, client.error_code, client.error_msg);
    }

    bool ok = dp.frames >= 1 && dp.failed_count == 0 && !client.display_error &&
              alive;
    khr_dmabuf_present_destroy(dev, &dp);
    khr_wl_client_disconnect(&client);
    return ok;
}

[[nodiscard]]
bool engine_init(void) {
    const char* banner = engine_get_banner();
    if (banner == nullptr) {
        return false;
    }
    printf("%s\n", banner);
    printf("Engine Version: %u.%u.%u\n", ENGINE_VERSION_MAJOR, ENGINE_VERSION_MINOR, ENGINE_VERSION_PATCH);
    printf("Standards Compliance: ISO C23 (__STDC_VERSION__ = %ldL)\n", __STDC_VERSION__);
    printf("Build Flag _GNU_SOURCE: Verified active\n");

    khr_topology_t topo = {};
    if (!khr_topology_init(&topo)) {
        printf("Topology: FAILED to bring up Ring A/B\n");
        return false;
    }

    printf("\n--- Wayplot Hardware Topology ---\n");
    printf("  Present CPU:  %d (pinned %d)\n", topo.present_cpu, topo.present_cpu_actual);
    printf("  Compute CPU:  %d (pinned %d)\n", topo.compute_cpu, topo.compute_cpu_actual);
    printf("  Ring A:       %u SQ / %u CQ  enter_fd=%u  clock=%s  no_iowait=%s\n",
           topo.ring_a.sq_entries, topo.ring_a.cq_entries, topo.ring_a.enter_fd,
           topo.clock_registered ? "CLOCK_MONOTONIC" : "none",
           topo.no_iowait ? "on" : "off");
    printf("  Ring B:       %u SQ / %u CQ  buffers=%s  hugepage=%zu KiB %s%s\n",
           topo.ring_b.sq_entries, topo.ring_b.cq_entries,
           topo.buffers_registered ? "registered" : "missing",
           topo.hugepage_sz / 1024,
           topo.hugepage_hugetlb ? "(MAP_HUGETLB)" : "(THP/anon)",
           topo.hugepage_collapsed ? " collapsed" : "");
    printf("  PBUF Tier 0:  %u x %u B (bgid %u)\n",
           topo.pbuf_tier0.entries, topo.pbuf_tier0.buf_size, topo.pbuf_tier0.bgid);
    printf("  PBUF Tier 1:  %u x %u B (bgid %u)\n",
           topo.pbuf_tier1.entries, topo.pbuf_tier1.buf_size, topo.pbuf_tier1.bgid);

    khr_gfx_device_t dev = {};
    khr_bda_arena_t arena = {};
    bool have_gpu = khr_gfx_device_init(&dev, (dev_t)0);
    bool wrapped = false;
    if (have_gpu) {
        wrapped = khr_bda_arena_init_with_host_ptr(&dev, &arena, topo.hugepage,
                                                   topo.hugepage_sz);
        if (!wrapped) {
            if (!khr_bda_arena_init(&dev, &arena, KHR_BDA_DEFAULT_ARENA_SZ)) {
                printf("  Vulkan:       device up, BDA arena FAILED\n");
                khr_gfx_device_destroy(&dev);
                have_gpu = false;
            }
        }
    }
    if (have_gpu) {
        printf("  Vulkan:       1.4 device up  BDA=0x%llx  %s\n",
               (unsigned long long)arena.gpu_address,
               wrapped ? "hugepage-imported (ingest==shader)"
                       : "separate UMA arena");
    } else {
        printf("  Vulkan:       skipped (no device)\n");
    }

    uint64_t bda = (have_gpu && arena.gpu_address != 0)
                       ? (uint64_t)arena.gpu_address
                       : (uint64_t)topo.hugepage;
    uint64_t got = 0;
    bool ipc_ok = khr_topology_signal_bda(&topo, bda) &&
                  khr_topology_wait_bda(&topo, &got, 1'000) &&
                  got == bda;
    printf("  MSG_RING IPC: %s (payload 0x%llx%s)\n",
           ipc_ok ? "OK" : "FAILED", (unsigned long long)got,
           (have_gpu && arena.gpu_address != 0) ? " VkDeviceAddress" : " host pointer");

    bool present_ok = true;
    if (ipc_ok && have_gpu && arena.gpu_address != 0) {
        present_ok = khr_engine_present(&topo, &dev, &arena);
    } else if (ipc_ok) {
        printf("  Present:      skipped (need GPU for DMA-BUF loop)\n");
    }

    if (have_gpu) {
        khr_bda_arena_destroy(&dev, &arena);
        khr_gfx_device_destroy(&dev);
    }
    khr_topology_destroy(&topo);
    return ipc_ok && present_ok;
}

#include "engine.h"
#include "khoros/core/topology.h"
#include "khoros/wayland/window.h"
#include "khoros/gfx/device.h"
#include "khoros/gfx/bda_arena.h"

#include <stdio.h>

[[nodiscard]]
const char* engine_get_banner(void) {
    static const char banner[] = "=== Khoros Engine (Linux 7.2 / C23 / io_uring / Vulkan 1.4) ===";
    return banner;
}

[[nodiscard]]
bool engine_init(const char* blob_path) {
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
        printf("  Split:        payload %zu KiB @0 / UI %zu KiB @%zu\n",
               khr_hp_payload_cap(arena.size) / 1024,
               KHR_HP_UI_RESERVE / 1024,
               khr_hp_ui_off(arena.size));
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
        present_ok = khr_window_run(&topo, &dev, &arena, blob_path);
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

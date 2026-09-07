#include "test_framework.h"
#include "mock_compositor.h"
#include "khoros/core/topology.h"
#include "khoros/uring/pbuf.h"
#include "khoros/uring/ring.h"
#include "khoros/wayland/client.h"
#include "khoros/wayland/xdg.h"
#include "khoros/wayland/dmabuf.h"
#include "khoros/wayland/dmabuf_present.h"
#include "khoros/gfx/device.h"
#include "khoros/gfx/dmabuf.h"
#include "khoros/gfx/bda_arena.h"
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <drm/drm.h>
#include <vulkan/vulkan.h>

/*
 * DMA-BUF zero-copy present loop + explicit syncobj points (ORIGINAL_REQUEST
 * R2): GPU-rendered slots, per-frame acquire/release timeline points, the
 * stall-free acquire bridge (counter query + TIMELINE_SIGNAL), release
 * recycling. The wire half runs on the mock through production paths; the
 * GPU half needs a real device and SKIPs honestly without one.
 */

/* Pure wire test: created/failed/release consume needs no GPU at all. */
[[nodiscard]]
bool test_dmabuf_present_created_failed_consume(void) {
    khr_dmabuf_present_t dp = {};
    dp.slots[0] = (khr_dmabuf_pslot_t){ .params_id = 40, .buffer_id = 41 };
    dp.slots[1] = (khr_dmabuf_pslot_t){ .params_id = 42, .buffer_id = 43 };

    /* created(slot 0) + failed(slot 1) in one batch, unknown traffic around. */
    khr_wl_msg_buf_t msg = {};
    khr_wl_buf_init(&msg);
    TEST_ASSERT(khr_wl_encode_header(&msg, 9999, 7, 8), "noise header failed");
    TEST_ASSERT(khr_wl_encode_header(&msg, 40, KHR_DMABUF_PARAMS_EVENT_CREATED, 12),
                "created header failed");
    TEST_ASSERT(khr_wl_encode_u32(&msg, 41), "created id failed");
    TEST_ASSERT(khr_wl_encode_header(&msg, 42, KHR_DMABUF_PARAMS_EVENT_FAILED, 8),
                "failed header failed");
    uint32_t n = khr_dmabuf_present_consume(&dp, msg.data, msg.size);
    TEST_ASSERT_EQ(n, 2U, "must handle exactly created + failed");
    TEST_ASSERT_EQ(dp.created_count, 1U, "one created expected");
    TEST_ASSERT_EQ(dp.failed_count, 1U, "one failed expected");
    TEST_ASSERT(dp.slots[0].created, "slot 0 must be created");
    TEST_ASSERT(!dp.slots[0].failed, "slot 0 must not be failed");
    TEST_ASSERT(dp.slots[1].failed, "slot 1 must be failed");

    /* A failed slot is never handed out again. */
    TEST_ASSERT_EQ(khr_dmabuf_present_next_free(&dp), 0U, "slot 0 free");
    dp.slots[0].busy = true;
    TEST_ASSERT_EQ(khr_dmabuf_present_next_free(&dp), UINT32_MAX,
                   "failed slot must not recycle");

    /* Release of the busy buffer frees exactly slot 0. */
    khr_wl_msg_buf_t rel = {};
    khr_wl_buf_init(&rel);
    TEST_ASSERT(khr_wl_encode_header(&rel, 41, KHR_WL_BUFFER_EVENT_RELEASE, 8),
                "release header failed");
    TEST_ASSERT_EQ(khr_dmabuf_present_consume(&dp, rel.data, rel.size), 1U,
                   "one release expected");
    TEST_ASSERT_EQ(dp.releases, 1U, "release counter must advance");
    TEST_ASSERT(!dp.slots[0].busy, "slot 0 must recycle");

    /* Unknown buffer releases and truncated tails are ignored, never crash. */
    khr_wl_msg_buf_t bogus = {};
    khr_wl_buf_init(&bogus);
    TEST_ASSERT(khr_wl_encode_header(&bogus, 7777, KHR_WL_BUFFER_EVENT_RELEASE, 8),
                "bogus header failed");
    TEST_ASSERT_EQ(khr_dmabuf_present_consume(&dp, bogus.data, bogus.size), 0U,
                   "bogus release must be ignored");
    uint8_t tail[5] = { 1, 2, 3, 4, 5 };
    TEST_ASSERT_EQ(khr_dmabuf_present_consume(&dp, tail, sizeof(tail)), 0U,
                   "truncated tail must be ignored");
    TEST_ASSERT_EQ(khr_dmabuf_present_consume(nullptr, msg.data, msg.size), 0U,
                   "null present must be safe");
    return true;
}

/* One pump harvest through xdg + dmabuf-present consumers. */
static void khr_test_dmp_pump(khr_topology_t* topo, khr_wl_client_t* client,
                              khr_xdg_shell_t* shell, khr_dmabuf_present_t* dp) {
    if (khr_topology_pump(topo, 500) == 0) {
        return;
    }
    for (;;) {
        khr_cqe_event_t evt = {};
        if (!khr_topology_pop_cqe(topo, &evt)) {
            break;
        }
        if (evt.res > 0) {
            uint16_t bid = (uint16_t)(evt.flags >> IORING_CQE_BUFFER_SHIFT);
            uint8_t* raw = khr_pbuf_data(&topo->pbuf_tier0, bid);
            khr_recvmsg_view_t view = {};
            if (raw != nullptr &&
                khr_recvmsg_parse(raw, topo->pbuf_tier0.buf_size, 0, 0, &view) &&
                view.payload != nullptr) {
                (void)khr_xdg_consume(client, shell, view.payload, view.payload_len);
                (void)khr_xdg_ack_pending(client, shell);
                if (dp != nullptr) {
                    (void)khr_dmabuf_present_consume(dp, view.payload,
                                                     view.payload_len);
                }
            }
        }
        khr_topology_recycle_cqe_buffer(topo, &evt);
    }
}

/* Full bringup shared by the GPU tests: mock + client + configured xdg +
 * device + arena + dmabuf present loop. Returns false (or SKIP-true) early. */
static bool khr_test_dmp_bringup(khr_topology_t* topo, mock_compositor_t* comp,
                                 khr_wl_client_t* client, khr_xdg_shell_t* shell,
                                 khr_gfx_device_t* dev, khr_bda_arena_t* arena,
                                 khr_dmabuf_present_t* dp) {
    if (!mock_compositor_init(comp)) {
        return false;
    }
    *client = (khr_wl_client_t){
        .sock_fd = comp->client_fd,
        .ring = &topo->ring_a,
    };
    khr_wl_client_attach_pbufs(client, &topo->pbuf_tier0, nullptr);
    khr_wl_client_attach_topology(client, topo);
    if (!mock_compositor_send_globals(comp)) {
        return false;
    }
    if (!mock_compositor_send_sync_done(comp, KHR_WL_CALLBACK_ID, 1)) {
        return false;
    }
    if (!khr_wl_client_roundtrip(client)) {
        return false;
    }
    *dev = (khr_gfx_device_t){};
    if (!khr_gfx_device_init(dev, (dev_t)0)) {
        TEST_SKIP("no Vulkan device in this environment");
    }
    *arena = (khr_bda_arena_t){};
    if (!khr_bda_arena_init(dev, arena, KHR_BDA_DEFAULT_ARENA_SZ)) {
        return false;
    }
    khr_xdg_init(shell);
    if (!khr_xdg_bind(client, shell)) {
        return false;
    }
    if (!khr_xdg_create_toplevel(client, shell, "Khoros", "khoros-engine")) {
        return false;
    }
    if (!khr_wl_client_arm_inbound(client)) {
        return false;
    }
    if (mock_compositor_drain(comp) < 5) {
        return false;
    }
    if (!mock_compositor_send_xdg_configure(comp, comp->xdg_toplevel_id,
                                            comp->xdg_surface_id, 0, 0, 12)) {
        return false;
    }
    khr_dmabuf_present_t early = {};
    khr_test_dmp_pump(topo, client, shell, &early);
    if (!shell->configured) {
        return false;
    }
    return khr_dmabuf_present_init(dev, client, shell->surface_id, 320, 240, dp);
}

static bool test_dmabuf_present_loop_mock_body(khr_topology_t* topo_ctx) {
/* `topo` aliases wrapper-owned storage so every TEST_ASSERT failure below
 * still unwinds through the wrapper's destroy: a red test can never strand
 * the worker thread on freed stack (cascade SEGV). Never declare a local
 * named topo or topo_ctx in here. */
#define topo (*topo_ctx)
    mock_compositor_t comp = {};
    khr_wl_client_t client = {};
    khr_xdg_shell_t shell = {};
    khr_gfx_device_t dev = {};
    khr_bda_arena_t arena = {};
    khr_dmabuf_present_t dp = {};
    bool brought = khr_test_dmp_bringup(&topo, &comp, &client, &shell, &dev,
                                        &arena, &dp);
    TEST_ASSERT(brought, "dmabuf present bringup failed");
    if (dev.device == VK_NULL_HANDLE) {
        mock_compositor_destroy(&comp);
        return true; /* SKIP path inside bringup */
    }

    /* Both imports crossed with real GPU fds; timelines: 1 acquire + 2 slot
     * release, all imported exactly once (no per-frame churn). Exact send
     * accounting: xdg ack_configure + 2 binds + get_surface + acquire import
     * + 2 x (params, add, immed, release import) = 13 messages. */
    TEST_ASSERT_EQ(mock_compositor_drain(&comp), 13, "exact init send count");
    TEST_ASSERT_EQ(comp.timeline_id_count, 3U, "three timelines expected");
    TEST_ASSERT(dp.acquire_tl_id != 0, "acquire timeline missing");
    TEST_ASSERT(dp.slots[0].buffer_id != 0 && dp.slots[1].buffer_id != 0,
                "both wl_buffers missing");
    TEST_ASSERT(dp.slots[0].buffer_id != dp.slots[1].buffer_id,
                "slot buffers must differ");

    /* Compositor confirms both imports; failed would retire instead. */
    TEST_ASSERT(mock_compositor_send_dmabuf_created(&comp, dp.slots[0].params_id,
                                                   dp.slots[0].buffer_id),
                "send created 0 failed");
    TEST_ASSERT(mock_compositor_send_dmabuf_created(&comp, dp.slots[1].params_id,
                                                   dp.slots[1].buffer_id),
                "send created 1 failed");
    khr_test_dmp_pump(&topo, &client, &shell, &dp);
    TEST_ASSERT_EQ(dp.created_count, 2U, "both slots must be created");

    /* Frame 0 → slot 0: GPU render, acquire point 1, release point 1.
     * Per commit: 2 point messages + attach/damage/commit = 5. */
    uint32_t commits_before = comp.commit_count;
    TEST_ASSERT(khr_dmabuf_present_commit_frame(&dev, &dp, &arena, 1),
                "commit frame 1 failed");
    TEST_ASSERT_EQ(mock_compositor_drain(&comp), 5, "commit send count");
    TEST_ASSERT_EQ(dp.frames, 1U, "frame counter must advance");
    TEST_ASSERT_EQ(comp.acquire_timeline_id, dp.acquire_tl_id,
                   "acquire must name our timeline");
    TEST_ASSERT_EQ(comp.acquire_point, 1U, "first acquire point must be 1");
    TEST_ASSERT_EQ(comp.release_timeline_id, dp.slots[0].release_tl_id,
                   "release must name slot 0 timeline");
    TEST_ASSERT_EQ(comp.release_point, 1U, "first release point must be 1");
    TEST_ASSERT(comp.commit_count > commits_before, "commit must land");
    TEST_ASSERT_EQ(comp.last_attached_buffer, dp.slots[0].buffer_id,
                   "slot 0 buffer must attach");
    TEST_ASSERT_EQ(dp.slots[0].gfx.painted, 1U, "slot 0 must render once");

    /* Frame 1 → slot 1 (round-robin), then both busy: refused, no block. */
    TEST_ASSERT(khr_dmabuf_present_commit_frame(&dev, &dp, &arena, 2),
                "commit frame 2 failed");
    TEST_ASSERT_EQ(mock_compositor_drain(&comp), 5, "commit send count");
    TEST_ASSERT_EQ(dp.slots[1].gfx.painted, 1U, "slot 1 must render once");
    TEST_ASSERT_EQ(comp.acquire_point, 2U, "acquire must advance to 2");
    TEST_ASSERT_EQ(khr_dmabuf_present_next_free(&dp), UINT32_MAX,
                   "no free slot expected");
    TEST_ASSERT(!khr_dmabuf_present_commit_frame(&dev, &dp, &arena, 3),
                "busy commit must fail without blocking");

    /* Release of buffer 0 recycles exactly slot 0; frame 2 reuses it with
     * advancing points on both timelines. */
    TEST_ASSERT(mock_compositor_send_buffer_release(&comp, dp.slots[0].buffer_id),
                "send release failed");
    khr_test_dmp_pump(&topo, &client, &shell, &dp);
    TEST_ASSERT_EQ(dp.releases, 1U, "one release expected");
    TEST_ASSERT_EQ(khr_dmabuf_present_next_free(&dp), 0U, "slot 0 recycles");
    TEST_ASSERT(khr_dmabuf_present_commit_frame(&dev, &dp, &arena, 3),
                "recommit slot 0 failed");
    TEST_ASSERT_EQ(mock_compositor_drain(&comp), 5, "commit send count");
    TEST_ASSERT_EQ(dp.frames, 3U, "three frames total");
    TEST_ASSERT_EQ(dp.slots[0].gfx.painted, 2U, "slot 0 rendered twice");
    TEST_ASSERT_EQ(comp.acquire_point, 3U, "acquire must advance to 3");
    TEST_ASSERT_EQ(comp.release_point, 2U, "slot 0 release must advance to 2");

    /* Teardown retires all three Wayland timelines; struct zeroes.
     * Destroy sends: per slot buffer + release-timeline, plus acquire. */
    khr_dmabuf_present_destroy(&dev, &dp);
    TEST_ASSERT_EQ(mock_compositor_drain(&comp), 5, "destroy send count");
    TEST_ASSERT_EQ(comp.timeline_destroy_count, 3U, "all timelines retired");
    TEST_ASSERT_EQ(dp.frames, 0U, "present struct must reset");

    khr_bda_arena_destroy(&dev, &arena);
    khr_gfx_device_destroy(&dev);
    mock_compositor_destroy(&comp);
#undef topo
    return true;
}

[[nodiscard]]
bool test_dmabuf_present_loop_mock(void) {
    khr_topology_t topo = {};
    if (!khr_topology_init(&topo)) {
        return false;
    }
    bool ok = test_dmabuf_present_loop_mock_body(&topo);
    khr_topology_destroy(&topo);
    return ok;
}

/* Acquire bridge: GPU completion becomes a DRM timeline point with no host
 * wait on any production path (the test itself may wait — it proves the
 * translation, not the pacing). */
static bool test_dmabuf_present_acquire_bridge_body(khr_topology_t* topo_ctx) {
#define topo (*topo_ctx)
    mock_compositor_t comp = {};
    khr_wl_client_t client = {};
    khr_xdg_shell_t shell = {};
    khr_gfx_device_t dev = {};
    khr_bda_arena_t arena = {};
    khr_dmabuf_present_t dp = {};
    bool brought = khr_test_dmp_bringup(&topo, &comp, &client, &shell, &dev,
                                        &arena, &dp);
    TEST_ASSERT(brought, "dmabuf present bringup failed");
    if (dev.device == VK_NULL_HANDLE) {
        mock_compositor_destroy(&comp);
        return true; /* SKIP path inside bringup */
    }

    /* Render frame 1 into slot 0 through the real commit path. */
    TEST_ASSERT(khr_dmabuf_present_commit_frame(&dev, &dp, &arena, 1),
                "commit frame 1 failed");
    uint64_t want = dev.acquire_point;
    TEST_ASSERT_EQ(want, 1U, "first point must be 1");

    /* Production never waits; the test does, once, to prove the bridge
     * translates a real GPU completion into a DRM point. */
    VkSemaphoreWaitInfo wi = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores = &dev.acquire_sem,
        .pValues = &want,
    };
    TEST_ASSERT(vkWaitSemaphores(dev.device, &wi, 5'000'000'000ULL) == VK_SUCCESS,
                "GPU completion wait failed");
    uint64_t pushed = khr_dmabuf_present_sync(&dev, &dp);
    TEST_ASSERT_EQ(pushed, want, "bridge must push the completed point");

    /* Non-blocking query observes the point (proves compositor-visible
     * state without any wait in the query itself). */
    uint64_t hs[1] = { dp.acquire_handle };
    uint64_t ps[1] = { want };
    struct drm_syncobj_timeline_array q = {};
    q.handles = (uint64_t)(uintptr_t)hs;
    q.points = (uint64_t)(uintptr_t)ps;
    q.count_handles = 1;
    q.flags = 0;
    TEST_ASSERT_EQ(ioctl(dev.drm_fd, DRM_IOCTL_SYNCOBJ_QUERY, &q), 0,
                   "timeline query failed");

    /* Monotonic: frame 2 advances both the device counter and the pushed
     * point through the same path. */
    TEST_ASSERT(khr_dmabuf_present_commit_frame(&dev, &dp, &arena, 2),
                "commit frame 2 failed");
    uint64_t want2 = dev.acquire_point;
    TEST_ASSERT_EQ(want2, 2U, "second point must be 2");
    VkSemaphoreWaitInfo wi2 = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores = &dev.acquire_sem,
        .pValues = &want2,
    };
    TEST_ASSERT(vkWaitSemaphores(dev.device, &wi2, 5'000'000'000ULL) == VK_SUCCESS,
                "GPU completion wait 2 failed");
    TEST_ASSERT_EQ(khr_dmabuf_present_sync(&dev, &dp), want2,
                   "bridge must advance to 2");

    khr_dmabuf_present_destroy(&dev, &dp);
    khr_bda_arena_destroy(&dev, &arena);
    khr_gfx_device_destroy(&dev);
    mock_compositor_destroy(&comp);
#undef topo
    return true;
}

[[nodiscard]]
bool test_dmabuf_present_acquire_bridge(void) {
    khr_topology_t topo = {};
    if (!khr_topology_init(&topo)) {
        return false;
    }
    bool ok = test_dmabuf_present_acquire_bridge_body(&topo);
    khr_topology_destroy(&topo);
    return ok;
}

[[nodiscard]]
bool test_dmabuf_present_resize_rejects_unbound(void) {
    khr_dmabuf_present_t dp = {};
    khr_gfx_device_t dev = {};
    TEST_ASSERT(!khr_dmabuf_present_resize(&dev, &dp, 100, 100),
                "unbound present must not resize");
    TEST_ASSERT(!khr_dmabuf_present_resize(nullptr, &dp, 640, 360),
                "null device must not resize");
    khr_dmabuf_present_drop_retired(nullptr, nullptr);
    return true;
}

/* Resize must not bind dmabuf/syncobj or get_surface again: a second
 * get_surface on the same wl_surface is already_constructed and is what
 * killed the live window on cursor-resize and side-tile. */
static bool test_dmabuf_present_resize_keeps_sync_surface_body(
    khr_topology_t* topo_ctx) {
#define topo (*topo_ctx)
    mock_compositor_t comp = {};
    khr_wl_client_t client = {};
    khr_xdg_shell_t shell = {};
    khr_gfx_device_t dev = {};
    khr_bda_arena_t arena = {};
    khr_dmabuf_present_t dp = {};
    bool brought = khr_test_dmp_bringup(&topo, &comp, &client, &shell, &dev,
                                        &arena, &dp);
    TEST_ASSERT(brought, "dmabuf present bringup failed");
    if (dev.device == VK_NULL_HANDLE) {
        mock_compositor_destroy(&comp);
        return true;
    }
    TEST_ASSERT_EQ(mock_compositor_drain(&comp), 13U, "exact init send count");
    TEST_ASSERT_EQ(comp.get_surface_count, 1U, "get_surface once at init");
    TEST_ASSERT_EQ(comp.dmabuf_bind_count, 1U, "dmabuf bind once at init");
    uint32_t surf = comp.syncobj_surface_id;
    uint32_t mgr = comp.client_syncobj_mgr_id;
    uint32_t dmabuf = comp.client_dmabuf_id;
    uint32_t acq = dp.acquire_tl_id;
    uint32_t buf0 = dp.slots[0].buffer_id;
    uint32_t buf1 = dp.slots[1].buffer_id;
    TEST_ASSERT(surf != 0 && acq != 0 && buf0 != 0 && buf1 != 0,
                "init endpoints missing");

    khr_gfx_device_wait_idle(&dev);
    TEST_ASSERT(khr_dmabuf_present_resize(&dev, &dp, 640, 360),
                "resize to 640x360 failed");
    TEST_ASSERT_EQ(dp.width, 640U, "resized width");
    TEST_ASSERT_EQ(dp.height, 360U, "resized height");
    TEST_ASSERT(dp.has_retiring, "old slots must retire after swap");
    TEST_ASSERT(dp.slots[0].buffer_id != buf0 && dp.slots[1].buffer_id != buf1,
                "new wl_buffers required");
    TEST_ASSERT_EQ(dp.acquire_tl_id, acq, "acquire timeline must be reused");
    TEST_ASSERT_EQ(mock_compositor_drain(&comp), 8U, "resize slot import count");
    TEST_ASSERT_EQ(comp.get_surface_count, 1U,
                   "resize must not get_surface again");
    TEST_ASSERT_EQ(comp.dmabuf_bind_count, 1U, "resize must not rebind dmabuf");
    TEST_ASSERT_EQ(comp.syncobj_surface_id, surf, "syncobj surface reused");
    TEST_ASSERT_EQ(comp.client_syncobj_mgr_id, mgr, "syncobj manager reused");
    TEST_ASSERT_EQ(comp.client_dmabuf_id, dmabuf, "dmabuf manager reused");
    TEST_ASSERT_EQ((uint32_t)comp.dma_width, 640U, "imported width");
    TEST_ASSERT_EQ((uint32_t)comp.dma_height, 360U, "imported height");

    TEST_ASSERT(khr_dmabuf_present_commit_frame(&dev, &dp, &arena, 1),
                "commit after resize failed");
    TEST_ASSERT_EQ(mock_compositor_drain(&comp), 5U, "commit send count");
    TEST_ASSERT_EQ(comp.last_attached_buffer, dp.slots[0].buffer_id,
                   "new-size buffer must attach");
    khr_dmabuf_present_drop_retired(&dev, &dp);
    TEST_ASSERT(!dp.has_retiring, "retired slots dropped");
    TEST_ASSERT_EQ(mock_compositor_drain(&comp), 4U, "retire send count");
    TEST_ASSERT_EQ(comp.get_surface_count, 1U, "drop must not get_surface");

    TEST_ASSERT(khr_dmabuf_present_resize(&dev, &dp, 640, 360),
                "same-size resize is a no-op success");
    TEST_ASSERT_EQ(mock_compositor_drain(&comp), 0U, "same-size sends nothing");

    khr_dmabuf_present_destroy(&dev, &dp);
    khr_bda_arena_destroy(&dev, &arena);
    khr_gfx_device_destroy(&dev);
    mock_compositor_destroy(&comp);
#undef topo
    return true;
}

[[nodiscard]]
bool test_dmabuf_present_resize_keeps_sync_surface(void) {
    khr_topology_t topo = {};
    if (!khr_topology_init(&topo)) {
        return false;
    }
    bool ok = test_dmabuf_present_resize_keeps_sync_surface_body(&topo);
    khr_topology_destroy(&topo);
    return ok;
}

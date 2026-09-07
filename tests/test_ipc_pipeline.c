#include "test_framework.h"
#include "mock_compositor.h"
#include "khoros/core/topology.h"
#include "khoros/core/cpu.h"
#include "khoros/gfx/device.h"
#include "khoros/gfx/bda_arena.h"
#include "khoros/wayland/client.h"
#include "khoros/wayland/xdg.h"
#include "khoros/wayland/present.h"
#include "khoros/wayland/dmabuf.h"
#include "khoros/wayland/syncobj.h"
#include "khoros/uring/ring.h"
#include <unistd.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <linux/time_types.h>

[[nodiscard]]
bool test_ipc_bda_stream_cross_core(void) {
    khr_topology_t topo = {};
    TEST_ASSERT(khr_topology_init(&topo), "Topology init failed");

    uint64_t test_bda = 0xDEADBEEFCAFE0000ULL;
    TEST_ASSERT(khr_topology_signal_bda(&topo, test_bda), "Signal BDA across cores failed");

    uint64_t received_bda = 0;
    TEST_ASSERT(khr_topology_wait_bda(&topo, &received_bda, 2'000), "Wait BDA on Core 0 failed");
    TEST_ASSERT_EQ(received_bda, test_bda, "Received BDA address mismatch");

    khr_topology_destroy(&topo);
    return true;
}

[[nodiscard]]
bool test_ipc_bda_high_throughput_burst(void) {
    khr_topology_t topo = {};
    TEST_ASSERT(khr_topology_init(&topo), "Topology init failed");

    constexpr uint32_t BURST_COUNT = 8;
    for (uint32_t i = 0; i < BURST_COUNT; i++) {
        uint64_t bda = 0x1000'0000ULL + ((uint64_t)i * 0x1000ULL);
        TEST_ASSERT(khr_topology_signal_bda(&topo, bda), "Signal BDA burst item");

        uint64_t recv_bda = 0;
        TEST_ASSERT(khr_topology_wait_bda(&topo, &recv_bda, 2'000), "Wait BDA burst item");
        TEST_ASSERT_EQ(recv_bda, bda, "Burst BDA mismatch");
    }

    khr_topology_destroy(&topo);
    return true;
}

[[nodiscard]]
bool test_ipc_ingestion_to_bda_pipeline(void) {
    /* 1. Create temporary test asset on disk */
    char path[256] = "/tmp/khr_asset_XXXXXX";
    int fd = mkstemp(path);
    TEST_ASSERT(fd >= 0, "mkstemp failed");

    uint8_t dummy_asset[8'192] = {};
    memset(dummy_asset, 0xAB, sizeof(dummy_asset));
    TEST_ASSERT_EQ(write(fd, dummy_asset, sizeof(dummy_asset)), (ssize_t)sizeof(dummy_asset), "write test asset");
    close(fd);

    /* 2. Ingest via Ring B 3-SQE hardlink */
    khr_topology_t topo = {};
    TEST_ASSERT(khr_topology_init(&topo), "Topology init failed");

    size_t bytes_read = 0;
    TEST_ASSERT(khr_topology_ingest(&topo, path, &bytes_read), "3-SQE ingestion failed");
    TEST_ASSERT_EQ(bytes_read, sizeof(dummy_asset), "Ingested bytes mismatch");

    /* Verify data was read directly into hugepage arena */
    TEST_ASSERT_NOT_NULL(topo.hugepage, "Hugepage arena must be mapped");
    TEST_ASSERT_EQ(((uint8_t*)topo.hugepage)[0], 0xAB, "Hugepage memory first byte verification");
    TEST_ASSERT_EQ(((uint8_t*)topo.hugepage)[sizeof(dummy_asset) - 1], 0xAB, "Hugepage memory last byte verification");

    /* 3. Real VkDeviceAddress of the SAME hugepage the ingest DMA'd into,
     * when the GPU can import it. Host-pointer IPC is only the no-GPU path. */
    khr_gfx_device_t dev = {};
    uint64_t want = (uint64_t)(uintptr_t)topo.hugepage;
    if (khr_gfx_device_init(&dev, (dev_t)0)) {
        khr_bda_arena_t arena = {};
        if (khr_bda_arena_init_with_host_ptr(&dev, &arena, topo.hugepage,
                                             topo.hugepage_sz)) {
            TEST_ASSERT_EQ(((uint8_t*)arena.host_ptr)[0], 0xAB,
                           "imported arena must see ingested bytes");
        } else {
            TEST_ASSERT(khr_bda_arena_init(&dev, &arena, KHR_BDA_DEFAULT_ARENA_SZ),
                        "fallback BDA arena");
        }
        TEST_ASSERT(arena.gpu_address != 0, "gpu address missing");
        want = (uint64_t)arena.gpu_address;
        TEST_ASSERT(khr_topology_signal_bda(&topo, want), "Signal BDA");
        uint64_t reaped_bda = 0;
        TEST_ASSERT(khr_topology_wait_bda(&topo, &reaped_bda, 2'000), "Wait BDA");
        TEST_ASSERT_EQ(reaped_bda, want, "Reaped VkDeviceAddress mismatch");
        khr_bda_arena_destroy(&dev, &arena);
        khr_gfx_device_destroy(&dev);
    } else {
        TEST_ASSERT(khr_topology_signal_bda(&topo, want), "Signal host pointer");
        uint64_t reaped_bda = 0;
        TEST_ASSERT(khr_topology_wait_bda(&topo, &reaped_bda, 2'000), "Wait BDA");
        TEST_ASSERT_EQ(reaped_bda, want, "Reaped host pointer mismatch");
    }

    khr_topology_destroy(&topo);
    unlink(path);
    return true;
}

[[nodiscard]]
bool test_tier3_bda_dmabuf_syncobj_pairwise(void) {
    /* Production encode path against the mock: not hand-rolled opcodes. */
    khr_topology_t topo = {};
    TEST_ASSERT(khr_topology_init(&topo), "topology init");

    mock_compositor_t comp = {};
    TEST_ASSERT(mock_compositor_init(&comp), "Mock compositor init failed");

    khr_wl_client_t client = {
        .sock_fd = comp.client_fd,
        .ring = &topo.ring_a,
    };
    khr_wl_client_attach_topology(&client, &topo);
    TEST_ASSERT(mock_compositor_send_globals(&comp), "globals");
    TEST_ASSERT(mock_compositor_send_sync_done(&comp, KHR_WL_CALLBACK_ID, 1), "sync");
    TEST_ASSERT(khr_wl_client_roundtrip(&client), "roundtrip");

    uint32_t dmabuf_id = 0;
    uint32_t mgr_id = 0;
    TEST_ASSERT(khr_dmabuf_bind(&client, &dmabuf_id), "dmabuf bind");
    TEST_ASSERT(khr_syncobj_bind(&client, &mgr_id), "syncobj bind");
    TEST_ASSERT(mock_compositor_drain(&comp) >= 2, "binds");

    int dma_fd = memfd_create("tier3_dmabuf", MFD_CLOEXEC);
    TEST_ASSERT(dma_fd >= 0, "memfd_create dma");
    TEST_ASSERT_EQ(ftruncate(dma_fd, 64 * 64 * 4), 0, "ftruncate dma");
    int sync_fd = memfd_create("tier3_syncobj", MFD_CLOEXEC);
    TEST_ASSERT(sync_fd >= 0, "memfd_create syncobj");

    uint32_t tl_id = 0;
    TEST_ASSERT(khr_syncobj_import_timeline(&client, mgr_id, sync_fd, &tl_id),
                "import timeline");
    uint32_t buf_id = 0;
    TEST_ASSERT(khr_dmabuf_import(&client, dmabuf_id, dma_fd, 64, 64,
                                 0x34325241U, 64 * 4, 0, 0, &buf_id),
                "dmabuf import");
    TEST_ASSERT(mock_compositor_drain(&comp) >= 4, "import requests");
    TEST_ASSERT(comp.timeline_id_count >= 1U, "timeline imported");
    TEST_ASSERT(comp.dma_plane_count >= 1U, "dma plane imported");
    (void)tl_id;
    (void)buf_id;

    close(dma_fd);
    close(sync_fd);
    mock_compositor_destroy(&comp);
    khr_topology_destroy(&topo);
    return true;
}

[[nodiscard]]
bool test_tier4_mock_wayland_frame_loop(void) {
    mock_compositor_t comp = {};
    TEST_ASSERT(mock_compositor_init(&comp), "Mock compositor init");

    /* 1. Server advertises globals */
    TEST_ASSERT(mock_compositor_send_globals(&comp), "send globals");

    /* 2. Client binds globals */
    comp.client_compositor_id = 10;
    comp.client_wm_base_id = 11;
    comp.client_dmabuf_id = 12;
    comp.client_syncobj_mgr_id = 13;
    comp.surface_id = 20;
    comp.xdg_surface_id = 21;
    comp.xdg_toplevel_id = 22;

    /* 3. Server sends initial configure event (1920x1080, serial=101) */
    TEST_ASSERT(mock_compositor_send_xdg_configure(&comp, 22, 21, 1920, 1080, 101), "send configure");

    /* 4. Client acknowledges configure (serial=101) */
    khr_wl_msg_buf_t ack = {};
    khr_wl_buf_init(&ack);
    TEST_ASSERT(khr_wl_encode_header(&ack, 21, KHR_XDG_SURFACE_ACK_CONFIGURE, 12), "ack configure");
    TEST_ASSERT(khr_wl_encode_u32(&ack, 101), "serial 101");
    TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, ack.data, ack.size, -1), "send ack");

    mock_compositor_drain(&comp);
    TEST_ASSERT_EQ(comp.last_ack_serial, 101U, "ack serial mismatch");

    /* Drain pending server messages (globals, configure) from client socket */
    uint8_t discard[4'096] = {};
    while (recv(comp.client_fd, discard, sizeof(discard), MSG_DONTWAIT) > 0) {}

    /* 5. Execute 5-frame presentation loop */
    constexpr uint32_t NUM_FRAMES = 5;
    for (uint32_t f = 1; f <= NUM_FRAMES; f++) {
        uint32_t buf_id = 100 + (f % 3); /* Triple buffer rotation */

        khr_wl_msg_buf_t frame_tx = {};
        khr_wl_buf_init(&frame_tx);

        /* wl_surface.attach(buffer, 0, 0) */
        TEST_ASSERT(khr_wl_encode_header(&frame_tx, 20, KHR_WL_SURFACE_ATTACH, 20), "attach");
        TEST_ASSERT(khr_wl_encode_u32(&frame_tx, buf_id), "buf_id");
        TEST_ASSERT(khr_wl_encode_i32(&frame_tx, 0), "x 0");
        TEST_ASSERT(khr_wl_encode_i32(&frame_tx, 0), "y 0");

        /* wl_surface.damage(0, 0, 1920, 1080) */
        TEST_ASSERT(khr_wl_encode_header(&frame_tx, 20, KHR_WL_SURFACE_DAMAGE, 24), "damage");
        TEST_ASSERT(khr_wl_encode_i32(&frame_tx, 0), "dx 0");
        TEST_ASSERT(khr_wl_encode_i32(&frame_tx, 0), "dy 0");
        TEST_ASSERT(khr_wl_encode_i32(&frame_tx, 1920), "dw 1920");
        TEST_ASSERT(khr_wl_encode_i32(&frame_tx, 1080), "dh 1080");

        /* wl_surface.commit() */
        TEST_ASSERT(khr_wl_encode_header(&frame_tx, 20, KHR_WL_SURFACE_COMMIT, 8), "commit");

        TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, frame_tx.data, frame_tx.size, -1), "send frame");

        mock_compositor_drain(&comp);
        TEST_ASSERT_EQ(comp.commit_count, f, "frame commit count");
        TEST_ASSERT_EQ(comp.last_attached_buffer, buf_id, "frame attached buffer");

        /* Compositor releases previous buffer */
        if (f > 1) {
            uint32_t prev_buf = 100 + ((f - 1) % 3);
            TEST_ASSERT(mock_compositor_send_buffer_release(&comp, prev_buf), "send release");
            uint8_t rx[16] = {};
            ssize_t n = recv(comp.client_fd, rx, sizeof(rx), 0);
            TEST_ASSERT_EQ(n, 8, "received buffer release event");
        }
    }

    mock_compositor_destroy(&comp);
    return true;
}

[[nodiscard]]
bool test_tier4_144hz_presentation_pacing(void) {
    /* Presentation pacing through the real topology: IORING_OP_POLL_ADD on an
     * eventfd (no poll/epoll) plus IORING_OP_TIMEOUT for the slice. This is
     * not vsync and does not claim 144 Hz — it proves the engine path. */
    khr_topology_t topo = {};
    TEST_ASSERT(khr_topology_init(&topo), "topology init");

    int efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    TEST_ASSERT(efd >= 0, "eventfd create failed");

    uint64_t t0 = khr_test_now_ns();
    uint32_t fired = 0;
    for (int frame = 0; frame < 3; frame++) {
        TEST_ASSERT(khr_topology_arm_eventfd(&topo, efd), "arm eventfd watch");
        uint64_t val = 1;
        TEST_ASSERT_EQ(write(efd, &val, sizeof(val)), (ssize_t)sizeof(val),
                       "eventfd signal");
        khr_cqe_event_t evt = {};
        TEST_ASSERT(khr_topology_wait_cqe(&topo, &evt, 1'000), "wait eventfd cqe");
        TEST_ASSERT_EQ(evt.user_data, KHR_TAG_EVENTFD, "eventfd tag");
        TEST_ASSERT(evt.res >= 0, "eventfd watch failed");
        fired++;
        /* Drain the counter so the next POLL_ADD can fire. */
        uint64_t drain = 0;
        (void)read(efd, &drain, sizeof(drain));
        struct __kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 1'000'000 };
        struct io_uring_sqe* to = khr_uring_prep_timeout(&topo.ring_a, &ts,
                                                         KHR_TAG_TIMEOUT);
        TEST_ASSERT_NOT_NULL(to, "timeout sqe");
        TEST_ASSERT(khr_uring_submit(&topo.ring_a, 0) >= 0, "timeout submit");
        khr_cqe_event_t tevt = {};
        TEST_ASSERT(khr_topology_wait_cqe(&topo, &tevt, 1'000), "wait timeout");
        TEST_ASSERT_EQ(tevt.user_data, KHR_TAG_TIMEOUT, "timeout tag");
    }
    uint64_t dt = khr_test_now_ns() - t0;
    TEST_ASSERT_EQ(fired, 3U, "three eventfd watches");
    TEST_ASSERT_GE(dt, 3'000'000ULL, "three 1 ms slices");
    TEST_ASSERT_LT(dt, 100'000'000ULL, "must not stall");

    close(efd);
    khr_topology_destroy(&topo);
    return true;
}

[[nodiscard]]
bool test_tier4_compositor_ping_keepalive_under_load(void) {
    mock_compositor_t comp = {};
    TEST_ASSERT(mock_compositor_init(&comp), "Mock compositor init");
    comp.client_wm_base_id = 11;
    comp.surface_id = 20;

    /* Simulate heavy frame commits while compositor sends async ping */
    for (uint32_t i = 1; i <= 3; i++) {
        /* Frame commit */
        khr_wl_msg_buf_t tx = {};
        khr_wl_buf_init(&tx);
        TEST_ASSERT(khr_wl_encode_header(&tx, 20, KHR_WL_SURFACE_COMMIT, 8), "commit");
        TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, tx.data, tx.size, -1), "send commit");

        /* Interleaved compositor ping */
        uint32_t ping_serial = 77000 + i;
        TEST_ASSERT(mock_compositor_send_ping(&comp, 11, ping_serial), "send ping");

        /* Client drains ping and replies pong */
        uint8_t rx[32] = {};
        ssize_t n = recv(comp.client_fd, rx, sizeof(rx), 0);
        TEST_ASSERT_EQ(n, 12, "ping message size");

        khr_wl_msg_buf_t pong = {};
        khr_wl_buf_init(&pong);
        TEST_ASSERT(khr_wl_encode_header(&pong, 11, KHR_XDG_WM_BASE_PONG, 12), "pong hdr");
        TEST_ASSERT(khr_wl_encode_u32(&pong, ping_serial), "pong serial");
        TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, pong.data, pong.size, -1), "send pong");

        mock_compositor_drain(&comp);
        TEST_ASSERT_EQ(comp.pong_count, i, "pong count");
        TEST_ASSERT_EQ(comp.last_pong_serial, ping_serial, "pong serial mismatch");
    }

    mock_compositor_destroy(&comp);
    return true;
}

[[nodiscard]]
bool test_tier4_window_resize_reconfiguration(void) {
    mock_compositor_t comp = {};
    TEST_ASSERT(mock_compositor_init(&comp), "Mock compositor init");
    comp.xdg_toplevel_id = 22;
    comp.xdg_surface_id = 21;
    comp.surface_id = 20;

    /* 1. Resize event from compositor: new dimensions 2560x1440, serial=303 */
    TEST_ASSERT(mock_compositor_send_xdg_configure(&comp, 22, 21, 2560, 1440, 303), "send resize configure");

    /* 2. Client acknowledges new configure serial */
    khr_wl_msg_buf_t ack = {};
    khr_wl_buf_init(&ack);
    TEST_ASSERT(khr_wl_encode_header(&ack, 21, KHR_XDG_SURFACE_ACK_CONFIGURE, 12), "ack resize");
    TEST_ASSERT(khr_wl_encode_u32(&ack, 303), "serial 303");
    TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, ack.data, ack.size, -1), "send ack");

    /* 3. Client retires 3 old swapchain timelines, emitting destroy (Opcode 0) for each.
     * Timelines are imported first: destroy matches live ids, as on the wire. */
    comp.client_syncobj_mgr_id = 90;
    for (uint32_t old_t = 500; old_t < 503; old_t++) {
        khr_wl_msg_buf_t imp = {};
        khr_wl_buf_init(&imp);
        TEST_ASSERT(khr_wl_encode_header(&imp, 90, KHR_SYNCOBJ_MGR_IMPORT_TIMELINE, 12), "import");
        TEST_ASSERT(khr_wl_encode_u32(&imp, old_t), "timeline id");
        TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, imp.data, imp.size, -1), "send import");
    }
    mock_compositor_drain(&comp);
    for (uint32_t old_t = 500; old_t < 503; old_t++) {
        khr_wl_msg_buf_t d = {};
        khr_wl_buf_init(&d);
        TEST_ASSERT(khr_wl_encode_header(&d, old_t, KHR_SYNCOBJ_TIMELINE_DESTROY, 8), "destroy old timeline");
        TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, d.data, d.size, -1), "send destroy");
        mock_compositor_drain(&comp);
    }
    TEST_ASSERT_EQ(comp.timeline_destroy_count, 3U, "old swapchain timelines destroyed");

    /* 4. Client commits resized buffer */
    khr_wl_msg_buf_t c = {};
    khr_wl_buf_init(&c);
    TEST_ASSERT(khr_wl_encode_header(&c, 20, KHR_WL_SURFACE_COMMIT, 8), "commit resized");
    TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, c.data, c.size, -1), "send commit");

    mock_compositor_drain(&comp);
    TEST_ASSERT_EQ(comp.last_ack_serial, 303U, "ack serial after resize");
    TEST_ASSERT_EQ(comp.commit_count, 1U, "commit count after resize");

    mock_compositor_destroy(&comp);
    return true;
}

[[nodiscard]]
bool test_tier4_full_dual_thread_e2e_pipeline(void) {
    /* Dual-thread e2e through production objects:
     * ingest on Ring B → wrap hugepage as BDA when a GPU exists → MSG_RING
     * the real VkDeviceAddress → XDG + shm present on the mock. DMA-BUF GPU
     * present is covered by test_dmabuf_present_loop_mock, not memfd theatre. */
    char test_path[256] = "/tmp/khr_e2e_asset_XXXXXX";
    int fd = mkstemp(test_path);
    TEST_ASSERT(fd >= 0, "mkstemp");
    uint8_t payload[4'096] = {};
    memset(payload, 0xEE, sizeof(payload));
    TEST_ASSERT_EQ(write(fd, payload, sizeof(payload)), (ssize_t)sizeof(payload), "write");
    close(fd);

    khr_topology_t topo = {};
    TEST_ASSERT(khr_topology_init(&topo), "Topology init");

    size_t bytes = 0;
    TEST_ASSERT(khr_topology_ingest(&topo, test_path, &bytes), "ingest");
    TEST_ASSERT_EQ(bytes, sizeof(payload), "ingested bytes");
    TEST_ASSERT_EQ(((uint8_t*)topo.hugepage)[0], 0xEE, "hugepage first byte");

    khr_gfx_device_t dev = {};
    khr_bda_arena_t arena = {};
    uint64_t want = (uint64_t)(uintptr_t)topo.hugepage;
    if (khr_gfx_device_init(&dev, (dev_t)0)) {
        if (khr_bda_arena_init_with_host_ptr(&dev, &arena, topo.hugepage,
                                             topo.hugepage_sz)) {
            TEST_ASSERT_EQ(((uint8_t*)arena.host_ptr)[0], 0xEE,
                           "imported BDA sees ingested bytes");
        } else {
            TEST_ASSERT(khr_bda_arena_init(&dev, &arena, KHR_BDA_DEFAULT_ARENA_SZ),
                        "fallback BDA arena");
        }
        want = (uint64_t)arena.gpu_address;
    }
    TEST_ASSERT(khr_topology_signal_bda(&topo, want), "signal bda");
    uint64_t reaped_bda = 0;
    TEST_ASSERT(khr_topology_wait_bda(&topo, &reaped_bda, 2'000), "wait bda");
    TEST_ASSERT_EQ(reaped_bda, want, "reaped bda");

    mock_compositor_t comp = {};
    TEST_ASSERT(mock_compositor_init(&comp), "Mock compositor init");
    khr_wl_client_t client = {
        .sock_fd = comp.client_fd,
        .ring = &topo.ring_a,
    };
    khr_wl_client_attach_pbufs(&client, &topo.pbuf_tier0, nullptr);
    khr_wl_client_attach_topology(&client, &topo);
    TEST_ASSERT(mock_compositor_send_globals(&comp), "globals");
    TEST_ASSERT(mock_compositor_send_sync_done(&comp, KHR_WL_CALLBACK_ID, 1), "sync");
    TEST_ASSERT(khr_wl_client_roundtrip(&client), "roundtrip");

    khr_xdg_shell_t shell = {};
    khr_xdg_init(&shell);
    TEST_ASSERT(khr_xdg_bind(&client, &shell), "xdg bind");
    TEST_ASSERT(khr_xdg_create_toplevel(&client, &shell, "Khoros", "khoros-engine"),
                "toplevel");
    TEST_ASSERT(khr_wl_client_arm_inbound(&client), "arm");
    TEST_ASSERT(mock_compositor_drain(&comp) >= 5, "xdg setup");
    TEST_ASSERT(mock_compositor_send_xdg_configure(&comp, comp.xdg_toplevel_id,
                                                   comp.xdg_surface_id, 0, 0, 1),
                "configure");
    for (int i = 0; i < 8 && !shell.configured; i++) {
        khr_cqe_event_t evt = {};
        if (!khr_topology_wait_cqe(&topo, &evt, 200)) {
            break;
        }
        const uint8_t* data = nullptr;
        size_t len = 0;
        bool rearm = false;
        (void)khr_wl_client_feed_cqe(&client, evt.user_data, evt.res, evt.flags,
                                     &data, &len, &rearm);
        if (data != nullptr && len >= 8) {
            (void)khr_xdg_consume(&client, &shell, data, len);
        }
        khr_topology_recycle_cqe_buffer(&topo, &evt);
        if (rearm) {
            (void)khr_wl_client_arm_inbound(&client);
        }
    }
    TEST_ASSERT(khr_xdg_can_attach(&shell), "attach gate");

    khr_present_t present = {};
    TEST_ASSERT(khr_present_init(&client, shell.surface_id, 64, 48, &present),
                "shm present init");
    TEST_ASSERT(mock_compositor_drain(&comp) >= 3, "shm setup");
    uint8_t* px = khr_present_slot_pixels(&present, 0);
    TEST_ASSERT_NOT_NULL(px, "pixels");
    khr_present_paint_test(&present, px, 1);
    uint32_t commits_before = comp.commit_count;
    TEST_ASSERT(khr_present_commit(&present, 0), "commit");
    TEST_ASSERT(mock_compositor_drain(&comp) >= 1, "commit on wire");
    TEST_ASSERT_EQ(comp.commit_count, commits_before + 1U,
                   "frame commit must increment (empty xdg commit already counted)");

    khr_present_destroy(&present);
    if (arena.buffer != VK_NULL_HANDLE) {
        khr_bda_arena_destroy(&dev, &arena);
    }
    if (dev.device != VK_NULL_HANDLE) {
        khr_gfx_device_destroy(&dev);
    }
    mock_compositor_destroy(&comp);
    khr_topology_destroy(&topo);
    unlink(test_path);
    return true;
}

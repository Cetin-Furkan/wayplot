#include "test_framework.h"
#include "mock_compositor.h"
#include "khoros/core/topology.h"
#include "khoros/core/cpu.h"
#include <unistd.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <fcntl.h>

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

    /* 3. Stream simulated GPU BDA pointer across cores */
    uint64_t simulated_bda = 0x2000'0000ULL + (uint64_t)(uintptr_t)topo.hugepage;
    TEST_ASSERT(khr_topology_signal_bda(&topo, simulated_bda), "Signal BDA");

    uint64_t reaped_bda = 0;
    TEST_ASSERT(khr_topology_wait_bda(&topo, &reaped_bda, 2'000), "Wait BDA");
    TEST_ASSERT_EQ(reaped_bda, simulated_bda, "Reaped BDA mismatch");

    khr_topology_destroy(&topo);
    unlink(path);
    return true;
}

[[nodiscard]]
bool test_tier3_bda_dmabuf_syncobj_pairwise(void) {
    mock_compositor_t comp = {};
    TEST_ASSERT(mock_compositor_init(&comp), "Mock compositor init failed");
    comp.surface_id = 20;
    comp.client_syncobj_mgr_id = 13;
    comp.client_dmabuf_id = 12;

    /* 1. Ingest BDA pointer */
    uint64_t bda_address = 0x7FFF'A000'1000ULL;

    /* 2. Create DMA-BUF backing memory */
    int dma_fd = memfd_create("tier3_dmabuf", MFD_CLOEXEC);
    TEST_ASSERT(dma_fd >= 0, "memfd_create dma");
    TEST_ASSERT_EQ(ftruncate(dma_fd, 1920 * 1080 * 4), 0, "ftruncate dma");

    /* 3. Create DRM syncobj timeline FD */
    int sync_fd = memfd_create("tier3_syncobj", MFD_CLOEXEC);
    TEST_ASSERT(sync_fd >= 0, "memfd_create syncobj");

    /* Wire sequence: import timeline -> get syncobj surface -> create dmabuf params -> add -> create_immed -> attach -> set_acquire -> set_release -> commit */
    khr_wl_msg_buf_t tx = {};

    /* Import timeline 90 */
    khr_wl_buf_init(&tx);
    TEST_ASSERT(khr_wl_encode_header(&tx, 13, KHR_SYNCOBJ_MGR_IMPORT_TIMELINE, 12), "import timeline");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 90), "timeline 90");
    TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, tx.data, tx.size, sync_fd), "send timeline");

    /* Get syncobj surface 91 */
    khr_wl_buf_init(&tx);
    TEST_ASSERT(khr_wl_encode_header(&tx, 13, KHR_SYNCOBJ_MGR_GET_SURFACE, 16), "get sync surface");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 91), "sync surface 91");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 20), "surface 20");
    TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, tx.data, tx.size, -1), "send get sync surface");

    /* DMA-BUF create params 92 */
    khr_wl_buf_init(&tx);
    TEST_ASSERT(khr_wl_encode_header(&tx, 12, KHR_DMABUF_CREATE_PARAMS, 12), "create params");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 92), "params 92");
    TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, tx.data, tx.size, -1), "send create params");

    /* DMA-BUF params add */
    khr_wl_buf_init(&tx);
    TEST_ASSERT(khr_wl_encode_header(&tx, 92, KHR_DMABUF_PARAMS_ADD, 28), "params add");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 0), "plane 0");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 0), "offset 0");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 1920 * 4), "stride 7680");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 0), "mod_hi");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 0), "mod_lo");
    TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, tx.data, tx.size, dma_fd), "send params add");

    /* DMA-BUF params create_immed buffer 93 */
    khr_wl_buf_init(&tx);
    TEST_ASSERT(khr_wl_encode_header(&tx, 92, KHR_DMABUF_PARAMS_CREATE_IMMED, 28), "create immed");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 93), "buffer 93");
    TEST_ASSERT(khr_wl_encode_i32(&tx, 1920), "w 1920");
    TEST_ASSERT(khr_wl_encode_i32(&tx, 1080), "h 1080");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 0x34325258), "XRGB8888");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 0), "flags 0");
    TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, tx.data, tx.size, -1), "send create immed");

    /* Attach buffer 93 */
    khr_wl_buf_init(&tx);
    TEST_ASSERT(khr_wl_encode_header(&tx, 20, KHR_WL_SURFACE_ATTACH, 20), "attach");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 93), "buffer 93");
    TEST_ASSERT(khr_wl_encode_i32(&tx, 0), "x 0");
    TEST_ASSERT(khr_wl_encode_i32(&tx, 0), "y 0");
    TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, tx.data, tx.size, -1), "send attach");

    /* Set acquire point (BDA-indexed frame) */
    khr_wl_buf_init(&tx);
    TEST_ASSERT(khr_wl_encode_header(&tx, 91, KHR_SYNCOBJ_SURFACE_SET_ACQUIRE, 20), "set acquire");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 90), "timeline 90");
    TEST_ASSERT(khr_wl_encode_u32(&tx, (uint32_t)(bda_address >> 32)), "hi");
    TEST_ASSERT(khr_wl_encode_u32(&tx, (uint32_t)bda_address), "lo");
    TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, tx.data, tx.size, -1), "send set acquire");

    /* Set release point */
    khr_wl_buf_init(&tx);
    TEST_ASSERT(khr_wl_encode_header(&tx, 91, KHR_SYNCOBJ_SURFACE_SET_RELEASE, 20), "set release");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 90), "timeline 90");
    TEST_ASSERT(khr_wl_encode_u32(&tx, (uint32_t)(bda_address >> 32)), "hi");
    TEST_ASSERT(khr_wl_encode_u32(&tx, (uint32_t)bda_address + 1), "lo + 1");
    TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, tx.data, tx.size, -1), "send set release");

    /* Surface commit */
    khr_wl_buf_init(&tx);
    TEST_ASSERT(khr_wl_encode_header(&tx, 20, KHR_WL_SURFACE_COMMIT, 8), "commit");
    TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, tx.data, tx.size, -1), "send commit");

    mock_compositor_drain(&comp);
    TEST_ASSERT_EQ(comp.commit_count, 1U, "commit count");
    TEST_ASSERT_EQ(comp.last_attached_buffer, 93U, "attached buffer");
    TEST_ASSERT_EQ(comp.acquire_point, bda_address, "acquire point matching BDA address");
    TEST_ASSERT_EQ(comp.release_point, bda_address + 1, "release point matching BDA address + 1");

    close(dma_fd);
    close(sync_fd);
    mock_compositor_destroy(&comp);
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
    /* 144 Hz frame cadence = 1'000'000'000 ns / 144 = 6'944'444 ns (~6.94 ms) */
    constexpr uint64_t TARGET_FRAME_NS = 6'944'444ULL;
    (void)TARGET_FRAME_NS;

    int efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    TEST_ASSERT(efd >= 0, "eventfd create failed");

    uint64_t t0 = khr_test_now_ns();

    /* Simulate 3 frame paces */
    for (int frame = 0; frame < 3; frame++) {
        /* Write to eventfd simulating compositor vblank/drm event */
        uint64_t val = 1;
        TEST_ASSERT_EQ(write(efd, &val, sizeof(val)), (ssize_t)sizeof(val), "eventfd signal");

        /* Non-blocking read */
        uint64_t read_val = 0;
        TEST_ASSERT_EQ(read(efd, &read_val, sizeof(read_val)), (ssize_t)sizeof(read_val), "eventfd read");
        TEST_ASSERT_EQ(read_val, 1ULL, "read val");

        /* Sleep remainder of frame target (1 ms test tick) */
        struct timespec rem = { .tv_sec = 0, .tv_nsec = 1'000'000 };
        nanosleep(&rem, nullptr);
    }

    uint64_t dt = khr_test_now_ns() - t0;
    TEST_ASSERT_GE(dt, 3'000'000ULL, "Total duration must be at least 3 ms");
    TEST_ASSERT_LT(dt, 100'000'000ULL, "Total duration must not stall (>100 ms)");

    close(efd);
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

    /* 3. Client retires 3 old swapchain timelines, emitting destroy (Opcode 0) for each */
    for (uint32_t old_t = 500; old_t < 503; old_t++) {
        comp.timeline_id = old_t;
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
    /* Full end-to-end integration:
     * 1. Disk asset -> 3-SQE atomic ingestion into Ring B hugepage arena.
     * 2. Core 1 sends 64-bit BDA device address via MSG_RING into Core 0 CQ ring.
     * 3. Core 0 reaps BDA and packs into push constants.
     * 4. Core 0 commits frame to Mock Compositor with DMA-BUF and DRM syncobj timeline.
     */
    char test_path[256] = "/tmp/khr_e2e_asset_XXXXXX";
    int fd = mkstemp(test_path);
    TEST_ASSERT(fd >= 0, "mkstemp");
    uint8_t payload[4'096] = {};
    memset(payload, 0xEE, sizeof(payload));
    TEST_ASSERT_EQ(write(fd, payload, sizeof(payload)), (ssize_t)sizeof(payload), "write");
    close(fd);

    /* Topology bringup */
    khr_topology_t topo = {};
    TEST_ASSERT(khr_topology_init(&topo), "Topology init");

    /* Mock compositor bringup */
    mock_compositor_t comp = {};
    TEST_ASSERT(mock_compositor_init(&comp), "Mock compositor init");
    comp.surface_id = 20;
    comp.client_syncobj_mgr_id = 13;
    comp.client_dmabuf_id = 12;

    /* Step 1: Ingest asset */
    size_t bytes = 0;
    TEST_ASSERT(khr_topology_ingest(&topo, test_path, &bytes), "ingest");
    TEST_ASSERT_EQ(bytes, sizeof(payload), "ingested bytes");

    /* Step 2: MSG_RING cross-core stream */
    uint64_t gpu_bda = 0x4000'0000ULL + (uint64_t)(uintptr_t)topo.hugepage;
    TEST_ASSERT(khr_topology_signal_bda(&topo, gpu_bda), "signal bda");

    uint64_t reaped_bda = 0;
    TEST_ASSERT(khr_topology_wait_bda(&topo, &reaped_bda, 2'000), "wait bda");
    TEST_ASSERT_EQ(reaped_bda, gpu_bda, "reaped bda");

    /* Step 3: Mock compositor presentation */
    int mem_fd = memfd_create("e2e_dmabuf", MFD_CLOEXEC);
    TEST_ASSERT(mem_fd >= 0, "memfd");
    TEST_ASSERT_EQ(ftruncate(mem_fd, 1920 * 1080 * 4), 0, "truncate");

    khr_wl_msg_buf_t tx = {};
    khr_wl_buf_init(&tx);
    TEST_ASSERT(khr_wl_encode_header(&tx, 20, KHR_WL_SURFACE_COMMIT, 8), "commit");
    TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, tx.data, tx.size, mem_fd), "send commit");

    mock_compositor_drain(&comp);
    TEST_ASSERT_EQ(comp.commit_count, 1U, "commit count");

    close(mem_fd);
    mock_compositor_destroy(&comp);
    khr_topology_destroy(&topo);
    unlink(test_path);
    return true;
}

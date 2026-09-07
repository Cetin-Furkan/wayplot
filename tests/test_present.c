#include "test_framework.h"
#include "mock_compositor.h"
#include "khoros/core/topology.h"
#include "khoros/uring/pbuf.h"
#include "khoros/uring/ring.h"
#include "khoros/wayland/client.h"
#include "khoros/wayland/xdg.h"
#include "khoros/wayland/shm.h"
#include "khoros/wayland/present.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>

static bool khr_test_same_file(int a, int b) {
    struct stat sa = {};
    struct stat sb = {};
    if (fstat(a, &sa) != 0 || fstat(b, &sb) != 0) {
        return false;
    }
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

[[nodiscard]]
static bool test_shm_wire_pool_and_buffer_body(khr_topology_t* topo_ctx) {
/* `topo` aliases wrapper-owned storage so every TEST_ASSERT failure below
 * still unwinds through the wrapper's destroy: a red test can never strand
 * the worker thread on freed stack (cascade SEGV). Never declare a local
 * named topo or topo_ctx in here. */
#define topo (*topo_ctx)
    mock_compositor_t comp = {};
    TEST_ASSERT(mock_compositor_init(&comp), "mock init failed");

    khr_wl_client_t client = {
        .sock_fd = comp.client_fd,
        .ring = &topo.ring_a,
    };
    khr_wl_client_attach_topology(&client, &topo);
    TEST_ASSERT(mock_compositor_send_globals(&comp), "send globals failed");
    TEST_ASSERT(mock_compositor_send_sync_done(&comp, KHR_WL_CALLBACK_ID, 1),
                "send sync done failed");
    TEST_ASSERT(khr_wl_client_roundtrip(&client), "registry roundtrip failed");
    TEST_ASSERT_NOT_NULL(khr_wl_client_find_global(&client, "wl_shm"), "wl_shm missing");

    uint32_t shm_id = 0;
    TEST_ASSERT(khr_shm_bind(&client, &shm_id), "shm bind failed");
    TEST_ASSERT(mock_compositor_drain(&comp) >= 1, "bind request missing");
    TEST_ASSERT_EQ(comp.client_shm_id, shm_id, "shm id mismatch");

    khr_shm_pool_t pool = {};
    TEST_ASSERT(khr_shm_pool_init(&client, shm_id, 8192, &pool), "pool init failed");
    TEST_ASSERT(pool.fd >= 0 && pool.addr != nullptr, "pool mapping failed");
    TEST_ASSERT(mock_compositor_drain(&comp) >= 1, "create_pool request missing");
    TEST_ASSERT_EQ(comp.shm_pool_id, pool.pool_id, "pool id mismatch");
    /* Pool FD crosses SCM_RIGHTS (dev/ino identity = shared memory, no copy). */
    bool fd_seen = false;
    for (uint32_t i = 0; i < comp.received_fd_count; i++) {
        if (khr_test_same_file(pool.fd, comp.received_fds[i])) {
            fd_seen = true;
        }
    }
    TEST_ASSERT(fd_seen, "pool fd must cross SCM_RIGHTS intact");
    int seals = fcntl(pool.fd, F_GET_SEALS);
    TEST_ASSERT((seals & F_SEAL_SHRINK) != 0 && (seals & F_SEAL_GROW) != 0,
                "pool must be sealed against truncation races");

    uint32_t buf0 = 0;
    uint32_t buf1 = 0;
    TEST_ASSERT(khr_shm_buffer_create(&client, &pool, 0, 32, 32, 128, &buf0),
                "buffer 0 failed");
    TEST_ASSERT(khr_shm_buffer_create(&client, &pool, 4096, 32, 32, 128, &buf1),
                "buffer 1 failed");
    TEST_ASSERT(mock_compositor_drain(&comp) >= 2, "create_buffer requests missing");
    TEST_ASSERT_EQ(comp.shm_buffer_count, 2U, "both buffers must be tracked");
    TEST_ASSERT_EQ(comp.shm_buffer_ids[0], buf0, "buffer 0 id mismatch");
    TEST_ASSERT_EQ(comp.shm_buffer_ids[1], buf1, "buffer 1 id mismatch");

    /* Overflowing the pool is rejected before any byte hits the wire. */
    uint32_t bad = 0;
    uint32_t reqs_before = comp.request_count;
    TEST_ASSERT(!khr_shm_buffer_create(&client, &pool, 8000, 32, 32, 128, &bad),
                "oversize buffer must be rejected");
    TEST_ASSERT_EQ(comp.request_count, reqs_before, "rejected buffer must not send");

    khr_shm_pool_destroy(&client, &pool);
    TEST_ASSERT_EQ(pool.fd, -1, "pool must reset after destroy");

    mock_compositor_destroy(&comp);
#undef topo
    return true;
}

[[nodiscard]]
bool test_shm_wire_pool_and_buffer(void) {
    khr_topology_t topo = {};
    if (!khr_topology_init(&topo)) {
        return false;
    }
    bool ok = test_shm_wire_pool_and_buffer_body(&topo);
    khr_topology_destroy(&topo);
    return ok;
}

/* Drain one pump harvest through both consumers (xdg + present). */
static void khr_test_present_pump(khr_topology_t* topo, khr_present_t* present,
                                  khr_wl_client_t* client, khr_xdg_shell_t* shell) {
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
                (void)khr_present_consume(present, view.payload, view.payload_len);
            }
        }
        khr_topology_recycle_cqe_buffer(topo, &evt);
    }
}

[[nodiscard]]
static bool test_present_loop_release_recycling_body(khr_topology_t* topo_ctx) {
#define topo (*topo_ctx)
    mock_compositor_t comp = {};
    TEST_ASSERT(mock_compositor_init(&comp), "mock init failed");

    khr_wl_client_t client = {
        .sock_fd = comp.client_fd,
        .ring = &topo.ring_a,
    };
    khr_wl_client_attach_pbufs(&client, &topo.pbuf_tier0, nullptr);
    khr_wl_client_attach_topology(&client, &topo);
    TEST_ASSERT(mock_compositor_send_globals(&comp), "send globals failed");
    TEST_ASSERT(mock_compositor_send_sync_done(&comp, KHR_WL_CALLBACK_ID, 1),
                "send sync done failed");
    TEST_ASSERT(khr_wl_client_roundtrip(&client), "registry roundtrip failed");

    /* XDG toplevel to CONFIGURED: attach gate open before any present call. */
    khr_xdg_shell_t shell = {};
    khr_xdg_init(&shell);
    TEST_ASSERT(khr_xdg_bind(&client, &shell), "xdg bind failed");
    TEST_ASSERT(khr_xdg_create_toplevel(&client, &shell, "Khoros", "khoros-engine"),
                "create toplevel failed");
    TEST_ASSERT(khr_wl_client_arm_inbound(&client), "arm inbound failed");
    /* Drain setup first so the mock learns the surface ids, then configure
     * with the real ids and harvest through the pump. */
    TEST_ASSERT(mock_compositor_drain(&comp) >= 5, "setup requests missing");
    TEST_ASSERT(mock_compositor_send_xdg_configure(&comp, comp.xdg_toplevel_id,
                                                   comp.xdg_surface_id, 0, 0, 12),
                "send configure failed");
    khr_present_t early = {};
    khr_test_present_pump(&topo, &early, &client, &shell);
    TEST_ASSERT(shell.configured, "shell must be configured");
    TEST_ASSERT(khr_xdg_can_attach(&shell), "attach gate must be open");

    /* Present loop on a small 64x48 stage (fast paint, exact assertions). */
    khr_present_t present = {};
    TEST_ASSERT(khr_present_init(&client, shell.surface_id, 64, 48, &present),
                "present init failed");
    TEST_ASSERT(mock_compositor_drain(&comp) >= 3, "shm setup requests missing");
    TEST_ASSERT_EQ(comp.shm_buffer_count, 2U, "two slot buffers expected");

    /* Frame 0 → slot 0: paint, verify pixels in the mapping, commit. */
    TEST_ASSERT_EQ(khr_present_next_free(&present), 0U, "slot 0 first");
    uint8_t* px0 = khr_present_slot_pixels(&present, 0);
    TEST_ASSERT_NOT_NULL(px0, "slot pixels null");
    khr_present_paint_test(&present, px0, 7);
    TEST_ASSERT_EQ(px0[0], 7U, "frame number in blue channel");
    TEST_ASSERT_EQ(px0[1], 0U, "top-left green must be 0");
    TEST_ASSERT_EQ(px0[2], 0U, "top-left red must be 0");
    TEST_ASSERT_EQ(px0[3], 255U, "alpha must be opaque");
    const uint8_t* br = px0 + ((size_t)47 * 64 + 63) * 4;
    TEST_ASSERT_EQ(br[1], 255U, "bottom-right green must be 255");
    TEST_ASSERT_EQ(br[2], 255U, "bottom-right red must be 255");
    uint32_t commits_before = comp.commit_count;
    TEST_ASSERT(khr_present_commit(&present, 0), "commit slot 0 failed");
    TEST_ASSERT_EQ(present.frames, 1U, "frame counter must advance");
    TEST_ASSERT(mock_compositor_drain(&comp) >= 1, "attach/commit missing");
    TEST_ASSERT_EQ(comp.last_attached_buffer, present.slots[0].buffer_id,
                   "slot 0 buffer must attach");
    TEST_ASSERT(comp.commit_count > commits_before, "commit must land");

    /* Frame 1 → slot 1 (round-robin, not slot 0). Then both busy: no slot,
     * commit refused without blocking. */
    TEST_ASSERT_EQ(khr_present_next_free(&present), 1U, "round-robin to slot 1");
    uint8_t* px1 = khr_present_slot_pixels(&present, 1);
    TEST_ASSERT_NOT_NULL(px1, "slot 1 pixels null");
    khr_present_paint_test(&present, px1, 8);
    TEST_ASSERT(khr_present_commit(&present, 1), "commit slot 1 failed");
    TEST_ASSERT_EQ(khr_present_next_free(&present), UINT32_MAX, "no free slot expected");
    TEST_ASSERT(!khr_present_commit(&present, 0), "busy slot commit must fail");

    /* Release of buffer 0 frees exactly slot 0 through the pump path. */
    TEST_ASSERT(mock_compositor_send_buffer_release(&comp, present.slots[0].buffer_id),
                "send release failed");
    khr_test_present_pump(&topo, &present, &client, &shell);
    TEST_ASSERT_EQ(present.releases, 1U, "one release expected");
    TEST_ASSERT_EQ(khr_present_next_free(&present), 0U, "slot 0 must recycle");

    /* Unknown release ids and double releases are ignored, never crash. */
    TEST_ASSERT(mock_compositor_send_buffer_release(&comp, 9999), "send bogus release failed");
    khr_test_present_pump(&topo, &present, &client, &shell);
    TEST_ASSERT_EQ(present.releases, 1U, "bogus release must be ignored");
    TEST_ASSERT(mock_compositor_send_buffer_release(&comp, present.slots[0].buffer_id),
                "send double release failed");
    khr_test_present_pump(&topo, &present, &client, &shell);
    TEST_ASSERT_EQ(present.releases, 1U, "double release of free slot ignored");

    /* Frame 2 reuses slot 0 with fresh pixels (B channel advances). */
    uint8_t* px2 = khr_present_slot_pixels(&present, 0);
    khr_present_paint_test(&present, px2, 9);
    TEST_ASSERT_EQ(px2[0], 9U, "recycled slot must carry new frame");
    TEST_ASSERT(khr_present_commit(&present, 0), "recommit slot 0 failed");
    TEST_ASSERT_EQ(present.frames, 3U, "three frames total");

    khr_present_destroy(&present);
    mock_compositor_destroy(&comp);
#undef topo
    return true;
}

[[nodiscard]]
bool test_present_loop_release_recycling(void) {
    khr_topology_t topo = {};
    if (!khr_topology_init(&topo)) {
        return false;
    }
    bool ok = test_present_loop_release_recycling_body(&topo);
    khr_topology_destroy(&topo);
    return ok;
}

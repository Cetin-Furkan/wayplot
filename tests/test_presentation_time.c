#include "test_framework.h"
#include "mock_compositor.h"
#include "khoros/wayland/presentation_time.h"
#include "khoros/uring/ring.h"
#include <string.h>
#include <sys/socket.h>

[[nodiscard]]
bool test_presentation_time_binding_and_lifecycle(void) {
    mock_compositor_t comp = {};
    TEST_ASSERT(mock_compositor_init(&comp), "mock_compositor_init");

    /* Send default 6 globals + wp_presentation as global 7 */
    TEST_ASSERT(mock_compositor_send_globals(&comp), "send_globals");
    TEST_ASSERT(mock_compositor_send_global(&comp, 7, KHR_WP_PRESENTATION_INTERFACE, 1), "send_global wp_presentation");
    TEST_ASSERT(mock_compositor_send_sync_done(&comp, KHR_WL_CALLBACK_ID, 1), "send_sync_done");

    khr_uring_config_t cfg = khr_uring_config_ring_a();
    khr_uring_t ring = {};
    TEST_ASSERT(khr_uring_init(&ring, &cfg), "ring init");

    khr_wl_client_t client = {
        .sock_fd = comp.client_fd,
        .ring = &ring,
    };
    TEST_ASSERT(khr_wl_client_roundtrip(&client), "client roundtrip");

    /* Verify wp_presentation global was discovered in registry */
    const khr_wl_global_t* g = khr_wl_client_find_global(&client, KHR_WP_PRESENTATION_INTERFACE);
    TEST_ASSERT_NOT_NULL(g, "wp_presentation global must be found");
    TEST_ASSERT_EQ(g->name, 7U, "global name must be 7");
    TEST_ASSERT_EQ(g->version, 1U, "global version must be 1");

    /* Bind wp_presentation */
    khr_presentation_time_t pt = {};
    TEST_ASSERT(khr_presentation_time_bind(&pt, &client), "presentation_time_bind");
    TEST_ASSERT(pt.bound, "pt.bound must be true");
    TEST_ASSERT_NE(pt.wp_presentation_id, 0U, "wp_presentation_id non-zero");

    /* Server drains bind */
    uint32_t drained = mock_compositor_drain(&comp);
    TEST_ASSERT_GE(drained, 1U, "drain bind");
    const mock_wl_request_t* bind_req = mock_compositor_find_request(&comp, KHR_WL_REGISTRY_ID, KHR_WL_REGISTRY_BIND);
    TEST_ASSERT_NOT_NULL(bind_req, "bind request found");

    /* Destroy wp_presentation */
    khr_presentation_time_destroy(&pt);
    TEST_ASSERT(!pt.bound, "pt.bound must be false after destroy");
    TEST_ASSERT_EQ(pt.wp_presentation_id, 0U, "wp_presentation_id 0 after destroy");

    khr_uring_destroy(&ring);
    mock_compositor_destroy(&comp);
    return true;
}

[[nodiscard]]
bool test_presentation_time_feedback_request_and_wire(void) {
    mock_compositor_t comp = {};
    TEST_ASSERT(mock_compositor_init(&comp), "mock_compositor_init");

    TEST_ASSERT(mock_compositor_send_globals(&comp), "send_globals");
    TEST_ASSERT(mock_compositor_send_global(&comp, 7, KHR_WP_PRESENTATION_INTERFACE, 1), "send_global wp_presentation");
    TEST_ASSERT(mock_compositor_send_sync_done(&comp, KHR_WL_CALLBACK_ID, 1), "send_sync_done");

    khr_uring_config_t cfg = khr_uring_config_ring_a();
    khr_uring_t ring = {};
    TEST_ASSERT(khr_uring_init(&ring, &cfg), "ring init");

    khr_wl_client_t client = {
        .sock_fd = comp.client_fd,
        .ring = &ring,
    };
    TEST_ASSERT(khr_wl_client_roundtrip(&client), "client roundtrip");

    khr_presentation_time_t pt = {};
    TEST_ASSERT(khr_presentation_time_bind(&pt, &client), "presentation_time_bind");

    /* Request feedback for surface 42, commit seq 100 */
    uint32_t fid = 0;
    constexpr uint32_t SURFACE_ID = 42;
    constexpr uint64_t COMMIT_SEQ = 100;
    TEST_ASSERT(khr_presentation_request_feedback(&pt, SURFACE_ID, COMMIT_SEQ, &fid), "request_feedback");
    TEST_ASSERT_NE(fid, 0U, "feedback_id non-zero");
    TEST_ASSERT_EQ(pt.pending_count, 1U, "pending_count must be 1");
    TEST_ASSERT_EQ(pt.pending[0].feedback_id, fid, "pending[0].feedback_id match");
    TEST_ASSERT_EQ(pt.pending[0].commit_seq, COMMIT_SEQ, "pending[0].commit_seq match");
    TEST_ASSERT(pt.pending[0].pending, "pending[0].pending is true");

    /* Server drains bind and feedback */
    uint32_t drained = mock_compositor_drain(&comp);
    TEST_ASSERT_GE(drained, 2U, "drain bind and feedback");

    const mock_wl_request_t* fb_req = mock_compositor_find_request(&comp, pt.wp_presentation_id, KHR_WP_PRESENTATION_FEEDBACK);
    TEST_ASSERT_NOT_NULL(fb_req, "feedback request found");
    TEST_ASSERT_EQ(fb_req->size, 16U, "feedback request wire size 16");

    uint32_t req_surf = 0;
    uint32_t req_fid = 0;
    size_t off = 0;
    TEST_ASSERT(khr_wl_decode_u32(fb_req->payload, fb_req->payload_len, &off, &req_surf), "decode surface_id");
    TEST_ASSERT(khr_wl_decode_u32(fb_req->payload, fb_req->payload_len, &off, &req_fid), "decode callback_id");
    TEST_ASSERT_EQ(req_surf, SURFACE_ID, "surface_id match");
    TEST_ASSERT_EQ(req_fid, fid, "feedback callback_id match");

    khr_presentation_time_destroy(&pt);
    khr_uring_destroy(&ring);
    mock_compositor_destroy(&comp);
    return true;
}

[[nodiscard]]
bool test_presentation_time_presented_parsing_and_metrics(void) {
    mock_compositor_t comp = {};
    TEST_ASSERT(mock_compositor_init(&comp), "mock_compositor_init");

    TEST_ASSERT(mock_compositor_send_globals(&comp), "send_globals");
    TEST_ASSERT(mock_compositor_send_global(&comp, 7, KHR_WP_PRESENTATION_INTERFACE, 1), "send_global");
    TEST_ASSERT(mock_compositor_send_sync_done(&comp, KHR_WL_CALLBACK_ID, 1), "send_sync_done");

    khr_uring_config_t cfg = khr_uring_config_ring_a();
    khr_uring_t ring = {};
    TEST_ASSERT(khr_uring_init(&ring, &cfg), "ring init");

    khr_wl_client_t client = {
        .sock_fd = comp.client_fd,
        .ring = &ring,
    };
    TEST_ASSERT(khr_wl_client_roundtrip(&client), "client roundtrip");

    khr_presentation_time_t pt = {};
    TEST_ASSERT(khr_presentation_time_bind(&pt, &client), "bind");

    /* Frame 1 */
    uint32_t fid1 = 0;
    TEST_ASSERT(khr_presentation_request_feedback(&pt, 1, 101, &fid1), "request_feedback 1");
    (void)mock_compositor_drain(&comp);

    /* Compositor sends presented event for Frame 1 */
    constexpr uint32_t SEC_LO = 50;
    constexpr uint32_t NSEC = 123'456'789;
    constexpr uint32_t REFRESH = 6'944'444; /* 144 Hz display */
    constexpr uint32_t SEQ_LO = 500;
    constexpr uint32_t FLAGS = KHR_WP_PRESENTATION_FEEDBACK_KIND_VSYNC |
                               KHR_WP_PRESENTATION_FEEDBACK_KIND_HW_CLOCK |
                               KHR_WP_PRESENTATION_FEEDBACK_KIND_HW_COMPLETION;

    TEST_ASSERT(mock_compositor_send_presentation_presented(&comp, fid1, 0, SEC_LO, NSEC, REFRESH, 0, SEQ_LO, FLAGS),
                "send presented 1");

    uint8_t rx[512] = {};
    ssize_t n = recv(client.sock_fd, rx, sizeof(rx), 0);
    TEST_ASSERT_GT(n, 0, "client recv presented 1");

    uint32_t processed = khr_presentation_time_consume(&pt, rx, (size_t)n);
    TEST_ASSERT_EQ(processed, 1U, "must process 1 event");
    TEST_ASSERT_EQ(pt.total_presented, 1U, "total_presented is 1");
    TEST_ASSERT_EQ(pt.last_sequence, 500ULL, "sequence is 500");
    TEST_ASSERT_EQ(pt.refresh_ns, REFRESH, "refresh_ns match");
    TEST_ASSERT_EQ(pt.flags, FLAGS, "flags match");
    constexpr uint64_t EXPECTED_TIME_NS = (uint64_t)SEC_LO * 1'000'000'000ULL + (uint64_t)NSEC;
    TEST_ASSERT_EQ(pt.last_presentation_time_ns, EXPECTED_TIME_NS, "presentation_time_ns match");
    TEST_ASSERT(pt.has_sample, "has_sample is true");
    TEST_ASSERT_EQ(pt.last_sample.commit_seq, 101ULL, "commit_seq match");
    TEST_ASSERT(pt.last_sample.presented, "sample.presented is true");
    TEST_ASSERT_EQ(pt.pending_count, 0U, "pending cleared");

    /* Frame 2: verify jitter calculation */
    uint32_t fid2 = 0;
    TEST_ASSERT(khr_presentation_request_feedback(&pt, 1, 102, &fid2), "request_feedback 2");
    (void)mock_compositor_drain(&comp);

    /* Frame 2 presentation: +6,950,000 ns (5,556 ns jitter) */
    uint32_t nsec2 = NSEC + 6'950'000;
    TEST_ASSERT(mock_compositor_send_presentation_presented(&comp, fid2, 0, SEC_LO, nsec2, REFRESH, 0, SEQ_LO + 1, FLAGS),
                "send presented 2");

    n = recv(client.sock_fd, rx, sizeof(rx), 0);
    TEST_ASSERT_GT(n, 0, "client recv presented 2");

    processed = khr_presentation_time_consume(&pt, rx, (size_t)n);
    TEST_ASSERT_EQ(processed, 1U, "process 2nd event");
    TEST_ASSERT_EQ(pt.total_presented, 2U, "total_presented is 2");
    TEST_ASSERT_EQ(pt.last_jitter_ns, 5'556LL, "jitter must be 6,950,000 - 6,944,444 = 5,556 ns");

    khr_presentation_time_destroy(&pt);
    khr_uring_destroy(&ring);
    mock_compositor_destroy(&comp);
    return true;
}

[[nodiscard]]
bool test_presentation_time_discarded_event(void) {
    mock_compositor_t comp = {};
    TEST_ASSERT(mock_compositor_init(&comp), "mock_compositor_init");

    TEST_ASSERT(mock_compositor_send_globals(&comp), "send_globals");
    TEST_ASSERT(mock_compositor_send_global(&comp, 7, KHR_WP_PRESENTATION_INTERFACE, 1), "send_global");
    TEST_ASSERT(mock_compositor_send_sync_done(&comp, KHR_WL_CALLBACK_ID, 1), "send_sync_done");

    khr_uring_config_t cfg = khr_uring_config_ring_a();
    khr_uring_t ring = {};
    TEST_ASSERT(khr_uring_init(&ring, &cfg), "ring init");

    khr_wl_client_t client = {
        .sock_fd = comp.client_fd,
        .ring = &ring,
    };
    TEST_ASSERT(khr_wl_client_roundtrip(&client), "client roundtrip");

    khr_presentation_time_t pt = {};
    TEST_ASSERT(khr_presentation_time_bind(&pt, &client), "bind");

    uint32_t fid = 0;
    TEST_ASSERT(khr_presentation_request_feedback(&pt, 1, 999, &fid), "request_feedback");
    (void)mock_compositor_drain(&comp);

    /* Compositor discards frame */
    TEST_ASSERT(mock_compositor_send_presentation_discarded(&comp, fid), "send discarded");

    uint8_t rx[256] = {};
    ssize_t n = recv(client.sock_fd, rx, sizeof(rx), 0);
    TEST_ASSERT_GT(n, 0, "client recv discarded");

    uint32_t processed = khr_presentation_time_consume(&pt, rx, (size_t)n);
    TEST_ASSERT_EQ(processed, 1U, "processed discarded event");
    TEST_ASSERT_EQ(pt.total_discarded, 1U, "total_discarded is 1");
    TEST_ASSERT_EQ(pt.pending_count, 0U, "pending cleared");
    TEST_ASSERT(pt.has_sample, "has_sample is true");
    TEST_ASSERT(!pt.last_sample.presented, "last_sample.presented is false");
    TEST_ASSERT_EQ(pt.last_sample.commit_seq, 999ULL, "commit_seq match");

    khr_presentation_time_destroy(&pt);
    khr_uring_destroy(&ring);
    mock_compositor_destroy(&comp);
    return true;
}

[[nodiscard]]
bool test_presentation_predict_next_vsync_deadline(void) {
    khr_presentation_time_t pt = {
        .bound = true,
        .refresh_ns = 8'333'333, /* 120 Hz */
        .last_presentation_time_ns = 1'000'000'000ULL,
    };

    uint64_t next_vsync = 0;
    uint32_t refresh = 0;

    /* 1. At t = 1,004,000,000 ns (mid-frame): next V-sync should be 1,008,333,333 ns */
    TEST_ASSERT(khr_presentation_predict_next_vsync(&pt, 1'004'000'000ULL, &next_vsync, &refresh), "predict 1");
    TEST_ASSERT_EQ(refresh, 8'333'333U, "refresh match");
    TEST_ASSERT_EQ(next_vsync, 1'008'333'333ULL, "next_vsync predicted timestamp");

    /* 2. At t = 1,018,000,000 ns (> 2 frames later): next V-sync should be 1,024,999,999 ns */
    TEST_ASSERT(khr_presentation_predict_next_vsync(&pt, 1'018'000'000ULL, &next_vsync, &refresh), "predict 2");
    /* elapsed = 18,000,000; periods = (18,000,000 / 8,333,333) + 1 = 3; 1,000,000,000 + 3 * 8,333,333 = 1,024,999,999 */
    TEST_ASSERT_EQ(next_vsync, 1'000'000'000ULL + 3ULL * 8'333'333ULL, "multi-frame vsync prediction");

    /* 3. Unbound / zero refresh must return false */
    khr_presentation_time_t unb = {};
    TEST_ASSERT(!khr_presentation_predict_next_vsync(&unb, 1'000'000'000ULL, &next_vsync, &refresh),
                "unbound must return false");

    return true;
}

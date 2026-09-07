#include "test_framework.h"
#include "khoros/core/topology.h"
#include "khoros/uring/pbuf.h"
#include "khoros/uring/ring.h"
#include "khoros/wayland/client.h"
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

/*
 * Send-await soundness tests (regression for the Mutter kill
 * "invalid arguments for wl_shm_pool.create_buffer").
 *
 * Root cause it pins down: send_skip/send_with_fd used to consume the NEXT
 * CQE blindly while enqueueing TWO (send + linked NOP). From the second send
 * on, the wait returned the previous call's NOP without confirming its own
 * bytes, so the caller's stack buffer was reused while the send was still in
 * flight — wire garbage the mock's timing masked and real Mutter rejected.
 * With an armed multishot inbound it was worse: the wait could eat a RECV
 * completion (reporting success) and silently drop a protocol packet.
 *
 * The fix under test: with an attached topology the send path awaits its OWN
 * KHR_TAG_WL_SEND completion with the exact byte count (one CQE per send, no
 * linked barrier), staging every foreign completion into the topology queues.
 * Zero CQE debt, zero steals.
 */

/* Drain exactly `want` stream bytes from the peer, collecting one SCM_RIGHTS
 * fd when the kernel attaches it. Blocking socketpair: bounded by the peer. */
static bool khr_test_peer_drain(int fd, uint8_t* out, size_t want, int* out_fd) {
    size_t have = 0;
    *out_fd = -1;
    while (have < want) {
        struct iovec iov = {
            .iov_base = out + have,
            .iov_len = want - have,
        };
        uint8_t cmsg[32] = {};
        struct msghdr msg = {
            .msg_iov = &iov,
            .msg_iovlen = 1,
            .msg_control = cmsg,
            .msg_controllen = sizeof(cmsg),
        };
        ssize_t n = recvmsg(fd, &msg, 0);
        if (n <= 0) {
            return false;
        }
        have += (size_t)n;
        for (struct cmsghdr* c = CMSG_FIRSTHDR(&msg); c != nullptr;
             c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS &&
                c->cmsg_len >= CMSG_LEN(sizeof(int))) {
                memcpy(out_fd, CMSG_DATA(c), sizeof(int));
            }
        }
    }
    return true;
}

/* Spin until at least one CQE is pending (bounded): makes the inbound-vs-send
 * interleave below deterministic instead of timing-dependent. The submit
 * flush is load-bearing, not paranoia: with DEFER_TASKRUN the kernel only
 * surfaces completions across an ENTER, so a peek-only spin would wait
 * forever. Submit harvests nothing — the completions stay pending, which is
 * exactly the interleave the sends below must survive. */
static bool khr_test_wait_pending(khr_topology_t* topo, uint32_t timeout_ms) {
    struct timespec start = {};
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        (void)khr_uring_submit(&topo->ring_a, 0);
        struct io_uring_cqe* cqe = nullptr;
        if (khr_uring_peek_cqe(&topo->ring_a, &cqe) && cqe != nullptr) {
            return true;
        }
        struct timespec now = {};
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t elapsed = ((now.tv_sec - start.tv_sec) * 1000LL) +
                          ((now.tv_nsec - start.tv_nsec) / 1'000'000LL);
        if (elapsed >= (int64_t)timeout_ms) {
            return false;
        }
    }
}

static bool test_send_await_no_debt_no_steal_body(khr_topology_t* topo_ctx) {
/* `topo` aliases wrapper-owned storage so every TEST_ASSERT failure below
 * still unwinds through the wrapper's destroy: a red test can never strand
 * the worker thread on freed stack (cascade SEGV). Never declare a local
 * named topo or topo_ctx in here. */
#define topo (*topo_ctx)
    int sv[2] = { -1, -1 };
    TEST_ASSERT(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0,
                "socketpair failed");

    khr_wl_client_t client = {
        .sock_fd = sv[0],
        .ring = &topo.ring_a,
    };
    khr_wl_client_attach_pbufs(&client, &topo.pbuf_tier0, nullptr);
    khr_wl_client_attach_topology(&client, &topo);
    TEST_ASSERT(khr_wl_client_arm_inbound(&client), "arm inbound failed");

    /* Inbound chatter lands FIRST and is guaranteed pending before our first
     * send: the old blind wait would eat one of these RECV completions here
     * (false success + a dropped protocol packet). */
    const char p0[] = "pkt00";
    const char p1[] = "pkt01";
    const char p2[] = "pkt02";
    TEST_ASSERT(send(sv[1], p0, sizeof(p0) - 1, 0) == (ssize_t)(sizeof(p0) - 1),
                "peer send 0 failed");
    TEST_ASSERT(send(sv[1], p1, sizeof(p1) - 1, 0) == (ssize_t)(sizeof(p1) - 1),
                "peer send 1 failed");
    TEST_ASSERT(send(sv[1], p2, sizeof(p2) - 1, 0) == (ssize_t)(sizeof(p2) - 1),
                "peer send 2 failed");
    TEST_ASSERT(khr_test_wait_pending(&topo, 2'000), "inbound never posted");

    /* Three sends through both paths while inbound completions interleave. */
    const char a[] = "msg-A";
    const char f[] = "FDMSG12";
    const char b[] = "msg-B";
    TEST_ASSERT(khr_wl_client_send_skip(&client, a, sizeof(a) - 1),
                "awaited send A failed");
    int null_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    TEST_ASSERT(null_fd >= 0, "open /dev/null failed");
    TEST_ASSERT(khr_wl_client_send_with_fd(&client, f, sizeof(f) - 1, null_fd),
                "awaited send-with-fd failed");
    close(null_fd);
    TEST_ASSERT(khr_wl_client_send_skip(&client, b, sizeof(b) - 1),
                "awaited send B failed");

    /* Harvest everything (the awaits already staged most of it; this only
     * drains stragglers): all 15 inbound bytes must surface, and no
     * SEND/NOP completion may be left parked (zero CQE debt). */
    (void)khr_topology_pump(&topo, 2'000);
    size_t inbound_bytes = 0;
    uint32_t debt = 0;
    uint32_t parked = 0;
    for (;;) {
        khr_cqe_event_t evt = {};
        if (!khr_topology_pop_cqe(&topo, &evt)) {
            break;
        }
        parked++;
        if (evt.user_data == KHR_TAG_WL_SEND || evt.user_data == KHR_TAG_NOP) {
            debt++;
        } else if ((evt.flags & IORING_CQE_F_BUFFER) != 0) {
            uint16_t bid = (uint16_t)(evt.flags >> IORING_CQE_BUFFER_SHIFT);
            khr_recvmsg_view_t view = {};
            if (khr_recvmsg_parse(khr_pbuf_data(&topo.pbuf_tier0, bid),
                                  topo.pbuf_tier0.buf_size, 0, 0, &view) &&
                view.payload != nullptr) {
                inbound_bytes += view.payload_len;
            }
            khr_topology_recycle_cqe_buffer(&topo, &evt);
        }
    }
    TEST_ASSERT(parked > 0, "expected parked completions");
    TEST_ASSERT_EQ(inbound_bytes, (size_t)15, "inbound packet bytes were stolen");
    TEST_ASSERT_EQ(debt, 0U, "send completions leaked into evt_q");

    /* The peer sees the exact ordered byte stream plus the passed fd. */
    uint8_t wire[17] = {};
    int got_fd = -1;
    TEST_ASSERT(khr_test_peer_drain(sv[1], wire, sizeof(wire), &got_fd),
                "peer drain failed");
    TEST_ASSERT_MEM_EQ(wire, "msg-AFDMSG12msg-B", sizeof(wire),
                       "byte stream reordered or corrupted");
    TEST_ASSERT(got_fd >= 0, "SCM_RIGHTS fd missing on peer");
    close(got_fd);

    TEST_ASSERT_EQ(khr_topology_staged_count(&topo), (size_t)0,
                   "evt_q must drain to 0");
    close(sv[0]);
    close(sv[1]);
#undef topo
    return true;
}

[[nodiscard]]
bool test_send_await_no_debt_no_steal(void) {
    khr_topology_t topo = {};
    if (!khr_topology_init(&topo)) {
        return false;
    }
    bool ok = test_send_await_no_debt_no_steal_body(&topo);
    khr_topology_destroy(&topo);
    return ok;
}

static bool test_await_tag_match_and_timeout_body(khr_topology_t* topo_ctx) {
#define topo (*topo_ctx)
    /* NULL handling: never crash, just refuse. */
    int32_t res = -999;
    TEST_ASSERT(!khr_topology_await_tag(nullptr, KHR_TAG_NOP, &res, 50),
                "null topo must fail");
    TEST_ASSERT(!khr_topology_await_tag(&topo, KHR_TAG_NOP, nullptr, 50),
                "null out_res must fail");

    /* Negative control: a tag nobody submitted must time out boundedly. */
    res = -999;
    struct timespec t0 = {};
    clock_gettime(CLOCK_MONOTONIC, &t0);
    TEST_ASSERT(!khr_topology_await_tag(&topo, 0xDEAD'BEEF'CAFE'F00DULL, &res,
                                       100),
                "unsubmitted tag must time out");
    struct timespec t1 = {};
    clock_gettime(CLOCK_MONOTONIC, &t1);
    int64_t waited = ((t1.tv_sec - t0.tv_sec) * 1000LL) +
                     ((t1.tv_nsec - t0.tv_nsec) / 1'000'000LL);
    TEST_ASSERT(waited >= 80 && waited < 2'000, "timeout must be bounded");
    TEST_ASSERT_EQ(res, -999, "timed-out await must not touch out_res");

    /* Positive control: a submitted tagged NOP is extracted with its res. */
    constexpr uint64_t kTag = 0x4157'4954'5445'5354ULL; /* 'AWITTEST' */
    TEST_ASSERT_NOT_NULL(khr_uring_prep_nop(&topo.ring_a, kTag),
                         "nop prep failed");
    TEST_ASSERT(khr_uring_submit(&topo.ring_a, 0) >= 0, "nop submit failed");
    int32_t got = -999;
    TEST_ASSERT(khr_topology_await_tag(&topo, kTag, &got, 2'000),
                "submitted tag must match");
    TEST_ASSERT_EQ(got, 0, "nop completion res must be 0");
    TEST_ASSERT_EQ(khr_topology_staged_count(&topo), (size_t)0,
                   "await must leave zero debt");
#undef topo
    return true;
}

[[nodiscard]]
bool test_await_tag_match_and_timeout(void) {
    khr_topology_t topo = {};
    if (!khr_topology_init(&topo)) {
        return false;
    }
    bool ok = test_await_tag_match_and_timeout_body(&topo);
    khr_topology_destroy(&topo);
    return ok;
}

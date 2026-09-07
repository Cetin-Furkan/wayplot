#include "test_framework.h"
#include "khoros/core/topology.h"
#include "khoros/core/cpu.h"
#include "khoros/uring/pbuf.h"
#include "khoros/uring/ring.h"
#include "khoros/wayland/client.h"
#include "khoros/wayland/wire.h"
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <string.h>

/*
 * Pump + wake + pin tests. Unlike the legacy staging tests, these assert the
 * new architecture directly: a single pump harvest classifies every pending
 * CQE into its FIFO, the worker wakes from a kernel-parked wait per signal,
 * and both threads stay off the kernel's cores 0/1.
 */

[[nodiscard]]
static bool test_cpu_pins_avoid_kernel_cores_body(khr_topology_t* topo_ctx) {
/* `topo` aliases wrapper-owned storage so every TEST_ASSERT failure below
 * still unwinds through the wrapper's destroy: a red test can never strand
 * the worker thread on freed stack (cascade SEGV). Never declare a local
 * named topo or topo_ctx in here. */
#define topo (*topo_ctx)
    /* Compile-time contract: presentation on 2, compute on 3. */
    static_assert(KHR_CPU_PRESENT == 2, "presentation must pin to core 2");
    static_assert(KHR_CPU_COMPUTE == 3, "compute must pin to core 3");

    TEST_ASSERT_EQ(topo.present_cpu, 2, "present cpu must be 2");
    TEST_ASSERT_EQ(topo.compute_cpu, 3, "compute cpu must be 3");
    TEST_ASSERT_EQ(topo.present_cpu_actual, 2, "present thread must run on core 2");
    TEST_ASSERT_EQ(topo.compute_cpu_actual, 3, "worker thread must run on core 3");
#undef topo
    return true;
}

[[nodiscard]]
bool test_cpu_pins_avoid_kernel_cores(void) {
    khr_topology_t topo = {};
    if (!khr_topology_init(&topo)) {
        return false;
    }
    bool ok = test_cpu_pins_avoid_kernel_cores_body(&topo);
    khr_topology_destroy(&topo);
    return ok;
}

[[nodiscard]]
static bool test_pump_classifies_no_swallow_body(khr_topology_t* topo_ctx) {
#define topo (*topo_ctx)
    int sv[2] = { -1, -1 };
    TEST_ASSERT(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0, "socketpair failed");
    int efd = eventfd(0, EFD_CLOEXEC);
    TEST_ASSERT(efd >= 0, "eventfd failed");

    struct msghdr rmsg = {};
    struct io_uring_sqe* rsqe = khr_uring_prep_recvmsg(&topo.ring_a, sv[0], &rmsg,
                                                      topo.pbuf_tier0.bgid, true,
                                                      KHR_TAG_RECVMSG);
    TEST_ASSERT_NOT_NULL(rsqe, "recvmsg prep failed");
    TEST_ASSERT(khr_uring_submit(&topo.ring_a, 0) >= 0, "recvmsg submit failed");
    TEST_ASSERT(khr_topology_arm_eventfd(&topo, efd), "arm eventfd failed");

    uint64_t one = 1;
    TEST_ASSERT(write(efd, &one, sizeof(one)) == (ssize_t)sizeof(one), "eventfd write failed");
    const char pkt[] = "pump-pbuf-payload";
    TEST_ASSERT(send(sv[1], pkt, sizeof(pkt) - 1, 0) == (ssize_t)(sizeof(pkt) - 1), "send failed");

    /* The BDA roundtrip must pair 1:1 while the two unrelated completions are
     * parked in evt_q (not dropped, not blocking the waiter). */
    constexpr uint64_t test_bda = 0xABCD'1234'DEAD'BEEFULL;
    TEST_ASSERT(khr_topology_signal_bda(&topo, test_bda), "signal BDA failed");
    uint64_t got_bda = 0;
    TEST_ASSERT(khr_topology_wait_bda(&topo, &got_bda, 2'000), "wait BDA failed");
    TEST_ASSERT_EQ(got_bda, test_bda, "BDA payload mismatch");

    /* One pump harvest must have classified both strays already. */
    TEST_ASSERT_EQ(khr_topology_staged_count(&topo), (size_t)2, "evt_q must park 2 events");

    bool saw_eventfd = false;
    bool saw_packet = false;
    for (uint32_t i = 0; i < 2; i++) {
        khr_cqe_event_t evt = {};
        TEST_ASSERT(khr_topology_pop_cqe(&topo, &evt), "pop parked event failed");
        if (evt.user_data == KHR_TAG_EVENTFD) {
            saw_eventfd = true;
        } else {
            khr_recvmsg_view_t view = {};
            uint16_t bid = (uint16_t)(evt.flags >> IORING_CQE_BUFFER_SHIFT);
            TEST_ASSERT(khr_recvmsg_parse(khr_pbuf_data(&topo.pbuf_tier0, bid),
                                          topo.pbuf_tier0.buf_size, 0, 0, &view),
                        "parse recvmsg_out failed");
            TEST_ASSERT_EQ(view.payload_len, (uint32_t)(sizeof(pkt) - 1), "payload len mismatch");
            TEST_ASSERT_MEM_EQ(view.payload, pkt, sizeof(pkt) - 1, "payload mismatch");
            saw_packet = true;
            khr_topology_recycle_cqe_buffer(&topo, &evt);
        }
    }
    TEST_ASSERT(saw_eventfd, "eventfd completion missing from evt_q");
    TEST_ASSERT(saw_packet, "PBUF packet missing from evt_q");
    TEST_ASSERT_EQ(khr_topology_staged_count(&topo), (size_t)0, "evt_q must drain to 0");

    close(sv[0]);
    close(sv[1]);
    close(efd);
#undef topo
    return true;
}

[[nodiscard]]
bool test_pump_classifies_no_swallow(void) {
    khr_topology_t topo = {};
    if (!khr_topology_init(&topo)) {
        return false;
    }
    bool ok = test_pump_classifies_no_swallow_body(&topo);
    khr_topology_destroy(&topo);
    return ok;
}

[[nodiscard]]
static bool test_pump_burst_pairs_wake_body(khr_topology_t* topo_ctx) {
#define topo (*topo_ctx)
    /* 20 back-to-back roundtrips: every signal doorbells the kernel-parked
     * worker via MSG_RING, every wait reaps exactly its own payload. A lost
     * wake or a swallowed CQE breaks pairing immediately. */
    for (uint64_t i = 1; i <= 20; i++) {
        uint64_t bda = 0xBDA0'0000'0000'0000ULL | i;
        TEST_ASSERT(khr_topology_signal_bda(&topo, bda), "burst signal failed");
        uint64_t got = 0;
        TEST_ASSERT(khr_topology_wait_bda(&topo, &got, 2'000), "burst wait failed");
        TEST_ASSERT_EQ(got, bda, "burst payload mismatch");
    }
    TEST_ASSERT_EQ(khr_topology_staged_count(&topo), (size_t)0, "no strays after burst");

#undef topo
    return true;
}

[[nodiscard]]
bool test_pump_burst_pairs_wake(void) {
    khr_topology_t topo = {};
    if (!khr_topology_init(&topo)) {
        return false;
    }
    bool ok = test_pump_burst_pairs_wake_body(&topo);
    khr_topology_destroy(&topo);
    return ok;
}

[[nodiscard]]
static bool test_wayland_pbuf_pump_drives_parse_body(khr_topology_t* topo_ctx) {
#define topo (*topo_ctx)
    int sv[2] = { -1, -1 };
    TEST_ASSERT(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0,
                "socketpair failed");

    /* Plain (non-direct) socket fd on the topology's Ring A, multishot PBUF
     * inbound armed exactly as the production loop arms it. */
    khr_wl_client_t client = {
        .sock_fd = sv[0],
        .ring = &topo.ring_a,
    };
    khr_wl_client_attach_pbufs(&client, &topo.pbuf_tier0, nullptr);
    khr_wl_client_attach_topology(&client, &topo);
    TEST_ASSERT(khr_wl_client_arm_inbound(&client), "multishot inbound arm failed");

    /* One server->client wl_registry.global event for wl_compositor. */
    khr_wl_msg_buf_t msg = {};
    khr_wl_buf_init(&msg);
    const char* iface = "wl_compositor";
    uint32_t str_len = 13; /* strlen + NUL */
    uint16_t total = (uint16_t)(8 + 4 + 4 + khr_wl_pad4(str_len) + 4);
    TEST_ASSERT(khr_wl_encode_header(&msg, KHR_WL_REGISTRY_ID,
                                     KHR_WL_REGISTRY_EVENT_GLOBAL, total),
                "encode global header failed");
    TEST_ASSERT(khr_wl_encode_u32(&msg, 7), "encode global name failed");
    TEST_ASSERT(khr_wl_encode_string(&msg, iface), "encode global iface failed");
    TEST_ASSERT(khr_wl_encode_u32(&msg, 4), "encode global version failed");
    TEST_ASSERT(send(sv[1], msg.data, msg.size, 0) == (ssize_t)msg.size,
                "peer send failed");

    /* Harvest exactly like the production loop: one kernel wait classifies
     * into evt_q, then the non-blocking consumer parses the PBUF packet. */
    uint32_t harvested = khr_topology_pump(&topo, 2'000);
    TEST_ASSERT(harvested >= 1, "pump harvested nothing");

    khr_cqe_event_t evt = {};
    TEST_ASSERT(khr_topology_pop_cqe(&topo, &evt), "pop inbound failed");
    uint32_t parsed = khr_wl_client_process_cqe(&client, evt.user_data,
                                                evt.res, evt.flags);
    TEST_ASSERT_EQ(parsed, 1U, "consumer must parse exactly 1 wire message");
    TEST_ASSERT_EQ(client.globals_count, 1U, "registry must hold 1 global");
    TEST_ASSERT_EQ(client.compositor_name, 7U, "compositor name mismatch");
    TEST_ASSERT_EQ(client.compositor_version, 4U, "compositor version mismatch");
    khr_topology_recycle_cqe_buffer(&topo, &evt);

    close(sv[0]);
    close(sv[1]);
#undef topo
    return true;
}

[[nodiscard]]
bool test_wayland_pbuf_pump_drives_parse(void) {
    khr_topology_t topo = {};
    if (!khr_topology_init(&topo)) {
        return false;
    }
    bool ok = test_wayland_pbuf_pump_drives_parse_body(&topo);
    khr_topology_destroy(&topo);
    return ok;
}

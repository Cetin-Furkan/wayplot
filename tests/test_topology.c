#include "test_framework.h"
#include "khoros/uring/ring.h"
#include "khoros/uring/pbuf.h"
#include "khoros/uring/ingest.h"
#include "khoros/core/topology.h"
#include "khoros/core/cpu.h"
#include "khoros/wayland/client.h"
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/mman.h>

[[nodiscard]]
static bool khr_test_drain_cqe(khr_uring_t* ring, uint32_t timeout_ms,
                               struct io_uring_cqe* out) {
    struct io_uring_cqe* cqe = nullptr;
    if (!khr_uring_wait_cqe_timeout(ring, &cqe, timeout_ms) || cqe == nullptr) {
        return false;
    }
    *out = *cqe;
    khr_uring_cqe_seen(ring, cqe);
    return true;
}

[[nodiscard]]
bool test_ring_a_clock_and_no_iowait(void) {
    khr_uring_config_t cfg = khr_uring_config_ring_a();
    khr_uring_t ring = {};
    TEST_ASSERT(khr_uring_init(&ring, &cfg), "Ring A setup failed");
    TEST_ASSERT((ring.enter_flags & IORING_ENTER_REGISTERED_RING) != 0,
                "REGISTER_RING_FDS must set IORING_ENTER_REGISTERED_RING");
    TEST_ASSERT((ring.setup_flags & IORING_SETUP_SINGLE_ISSUER) != 0,
                "Ring A must be SINGLE_ISSUER");
    TEST_ASSERT((ring.setup_flags & IORING_SETUP_DEFER_TASKRUN) != 0,
                "Ring A must be DEFER_TASKRUN");
    TEST_ASSERT((ring.setup_flags & IORING_SETUP_NO_SQARRAY) != 0,
                "Ring A must be NO_SQARRAY");
    TEST_ASSERT((ring.features & IORING_FEAT_SINGLE_MMAP) != 0,
                "Kernel must advertise SINGLE_MMAP");

    TEST_ASSERT(khr_uring_register_clock(&ring, CLOCK_MONOTONIC),
                "IORING_REGISTER_CLOCK CLOCK_MONOTONIC failed");
    TEST_ASSERT(ring.clock_monotonic, "clock_monotonic flag not set");

    khr_uring_enable_no_iowait(&ring);
    if ((ring.features & IORING_FEAT_NO_IOWAIT) != 0) {
        TEST_ASSERT((ring.enter_flags & IORING_ENTER_NO_IOWAIT) != 0,
                    "NO_IOWAIT enter flag must be set when the kernel advertises it");
    }

    struct __kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 1'000'000 };
    TEST_ASSERT(khr_uring_prep_timeout(&ring, &ts, KHR_TAG_TIMEOUT) != nullptr,
                "TIMEOUT SQE allocation failed");
    struct io_uring_cqe cqe = {};
    TEST_ASSERT(khr_test_drain_cqe(&ring, 500, &cqe), "TIMEOUT CQE not received");
    TEST_ASSERT(cqe.user_data == KHR_TAG_TIMEOUT, "TIMEOUT user_data mismatch");
    TEST_ASSERT(cqe.res == -ETIME, "TIMEOUT must complete with -ETIME");

    khr_uring_destroy(&ring);
    return true;
}

[[nodiscard]]
bool test_msg_ring_64bit_payload(void) {
    khr_uring_t ring_a = {};
    khr_uring_t ring_b = {};
    khr_uring_config_t cfg_a = khr_uring_config_ring_a();
    khr_uring_config_t cfg_b = khr_uring_config_ring_b();
    TEST_ASSERT(khr_uring_init(&ring_a, &cfg_a), "Ring A init failed");
    TEST_ASSERT(khr_uring_init(&ring_b, &cfg_b), "Ring B init failed");
    TEST_ASSERT(ring_b.sq_entries == KHR_RING_B_SQ_ENTRIES, "Ring B SQ size mismatch");
    TEST_ASSERT(ring_b.cq_entries == KHR_RING_B_CQ_ENTRIES, "Ring B CQ size mismatch");

    constexpr uint64_t payload = 0xDEAD'BEEF'CAFE'BABEULL;
    struct io_uring_sqe* sqe = khr_uring_prep_msg_ring(&ring_b, ring_a.ring_fd,
                                                       payload, KHR_MSG_RES_BDA);
    TEST_ASSERT(sqe != nullptr, "MSG_RING SQE allocation failed");
    sqe->flags |= IOSQE_IO_LINK;
    TEST_ASSERT(khr_uring_prep_nop(&ring_b, KHR_TAG_NOP) != nullptr, "NOP barrier failed");

    struct io_uring_cqe src = {};
    TEST_ASSERT(khr_test_drain_cqe(&ring_b, 1'000, &src), "Ring B NOP CQE missing");
    TEST_ASSERT(src.user_data == KHR_TAG_NOP, "Expected NOP barrier CQE on Ring B");
    TEST_ASSERT(src.res >= 0, "NOP barrier failed");

    struct io_uring_cqe dst = {};
    TEST_ASSERT(khr_test_drain_cqe(&ring_a, 1'000, &dst), "Ring A did not receive MSG_RING CQE");
    TEST_ASSERT(dst.user_data == payload, "MSG_RING 64-bit payload mismatch");
    TEST_ASSERT(dst.res == (int32_t)KHR_MSG_RES_BDA, "MSG_RING res tag mismatch");

    khr_uring_destroy(&ring_b);
    khr_uring_destroy(&ring_a);
    return true;
}

[[nodiscard]]
bool test_pbuf_multishot_recvmsg(void) {
    int sv[2] = { -1, -1 };
    TEST_ASSERT(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0,
                "socketpair failed");

    khr_uring_t ring = {};
    khr_uring_config_t cfg = khr_uring_config_ring_a();
    TEST_ASSERT(khr_uring_init(&ring, &cfg), "Ring A init failed");

    khr_pbuf_t pbuf = {};
    TEST_ASSERT(khr_pbuf_init(&pbuf, &ring, KHR_PBUF_TIER0_BGID,
                              KHR_PBUF_TIER0_COUNT, KHR_PBUF_TIER0_SIZE),
                "PBUF tier-0 registration failed");

    struct msghdr hdr = {};
    TEST_ASSERT(khr_uring_prep_recvmsg(&ring, sv[0], &hdr, pbuf.bgid, true,
                                       KHR_TAG_RECVMSG) != nullptr,
                "RECVMSG SQE failed");
    TEST_ASSERT(khr_uring_submit(&ring, 0) >= 0, "RECVMSG submit failed");

    const char msg[] = "wayplot-hot";
    TEST_ASSERT(send(sv[1], msg, sizeof(msg) - 1, 0) == (ssize_t)(sizeof(msg) - 1),
                "send failed");

    struct io_uring_cqe cqe = {};
    TEST_ASSERT(khr_test_drain_cqe(&ring, 1'000, &cqe), "multishot RECVMSG CQE missing");
    TEST_ASSERT(cqe.res >= 0, "RECVMSG failed");
    TEST_ASSERT((cqe.flags & IORING_CQE_F_BUFFER) != 0, "CQE must carry a provided buffer");

    uint16_t bid = khr_cqe_buf_id(&cqe);
    uint8_t* buf = khr_pbuf_data(&pbuf, bid);
    TEST_ASSERT(buf != nullptr, "PBUF bid out of range");

    khr_recvmsg_view_t view = {};
    TEST_ASSERT(khr_recvmsg_parse(buf, pbuf.buf_size, 0, 0, &view),
                "recvmsg_out parse failed");
    TEST_ASSERT(view.payload_len == sizeof(msg) - 1, "payload length mismatch");
    TEST_ASSERT(memcmp(view.payload, msg, sizeof(msg) - 1) == 0, "payload bytes mismatch");

    khr_pbuf_recycle(&pbuf, bid);
    khr_pbuf_destroy(&pbuf);
    khr_uring_destroy(&ring);
    close(sv[0]);
    close(sv[1]);
    return true;
}

[[nodiscard]]
bool test_sendmsg_skip_success(void) {
    int sv[2] = { -1, -1 };
    TEST_ASSERT(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0,
                "socketpair failed");

    khr_uring_t ring = {};
    khr_uring_config_t cfg = khr_uring_config_ring_a();
    TEST_ASSERT(khr_uring_init(&ring, &cfg), "Ring A init failed");

    char payload[] = "skip-cqe";
    struct iovec iov = { .iov_base = payload, .iov_len = sizeof(payload) - 1 };
    struct msghdr hdr = { .msg_iov = &iov, .msg_iovlen = 1 };

    struct io_uring_sqe* sqe = khr_uring_prep_sendmsg(&ring, sv[0], &hdr, true, KHR_TAG_SENDMSG);
    TEST_ASSERT(sqe != nullptr, "SENDMSG SQE failed");
    sqe->flags |= IOSQE_IO_LINK;
    TEST_ASSERT(khr_uring_prep_nop(&ring, KHR_TAG_NOP) != nullptr, "NOP barrier failed");

    struct io_uring_cqe cqe = {};
    TEST_ASSERT(khr_test_drain_cqe(&ring, 1'000, &cqe), "NOP CQE missing");
    TEST_ASSERT(cqe.user_data == KHR_TAG_NOP,
                "SENDMSG with CQE_SKIP_SUCCESS must not post a success CQE");
    TEST_ASSERT(cqe.res >= 0, "NOP failed");

    char recvbuf[32] = {};
    ssize_t n = recv(sv[1], recvbuf, sizeof(recvbuf), 0);
    TEST_ASSERT(n == (ssize_t)(sizeof(payload) - 1), "peer did not receive SENDMSG bytes");
    TEST_ASSERT(memcmp(recvbuf, payload, sizeof(payload) - 1) == 0, "SENDMSG payload mismatch");

    khr_uring_destroy(&ring);
    close(sv[0]);
    close(sv[1]);
    return true;
}

[[nodiscard]]
bool test_topology_bringup_and_ipc(void) {
    khr_topology_t topo = {};
    TEST_ASSERT(khr_topology_init(&topo), "topology init failed");

    bool pin_present = topo.present_cpu_actual == topo.present_cpu;
    bool pin_compute = topo.compute_cpu_actual == topo.compute_cpu;
    bool huge_ok = topo.hugepage != nullptr && topo.hugepage_sz == KHR_HUGEPAGE_SZ;
    bool buf_ok = topo.buffers_registered;
    bool clock_ok = topo.clock_registered;
    bool t0_ok = topo.pbuf_tier0.entries == KHR_PBUF_TIER0_COUNT &&
                 topo.pbuf_tier0.buf_size == KHR_PBUF_TIER0_SIZE;
    bool t1_ok = topo.pbuf_tier1.entries == KHR_PBUF_TIER1_COUNT &&
                 topo.pbuf_tier1.buf_size == KHR_PBUF_TIER1_SIZE;
    bool reg_ok = (topo.ring_a.enter_flags & IORING_ENTER_REGISTERED_RING) != 0;
    bool std_ok = topo.std_page != nullptr;

    uint64_t bda = (uint64_t)topo.hugepage;
    bool sig_ok = khr_topology_signal_bda(&topo, bda);
    uint64_t got = 0;
    bool wait_ok = sig_ok && khr_topology_wait_bda(&topo, &got, 1'000);
    bool match = wait_ok && got == bda;
    khr_topology_destroy(&topo);

    TEST_ASSERT(pin_present, "Thread 1 is not pinned to the presentation core");
    TEST_ASSERT(pin_compute, "Thread 2 is not pinned to the compute core");
    TEST_ASSERT(huge_ok, "2 MiB hugepage arena missing");
    TEST_ASSERT(buf_ok, "Ring B REGISTER_BUFFERS failed");
    TEST_ASSERT(clock_ok, "Ring A CLOCK_MONOTONIC registration failed");
    TEST_ASSERT(t0_ok, "tier-0 pbuf mismatch");
    TEST_ASSERT(t1_ok, "tier-1 pbuf mismatch");
    TEST_ASSERT(reg_ok, "Ring A must use a registered ring fd");
    TEST_ASSERT(std_ok, "4 KiB standard page missing");
    TEST_ASSERT(sig_ok, "worker MSG_RING submit failed");
    TEST_ASSERT(wait_ok, "Ring A did not harvest BDA CQE");
    TEST_ASSERT(match, "BDA pointer did not survive MSG_RING IPC");
    return true;
}

[[nodiscard]]
bool test_topology_ingest_read_fixed(void) {
    khr_topology_t topo = {};
    TEST_ASSERT(khr_topology_init(&topo), "topology init failed");

    char path[] = "/tmp/khoros_ingest_XXXXXX";
    int fd = mkstemp(path);
    TEST_ASSERT(fd >= 0, "mkstemp failed");

    uint8_t pattern[4'096];
    for (uint32_t i = 0; i < sizeof(pattern); i++) {
        pattern[i] = (uint8_t)(i * 17U);
    }
    TEST_ASSERT(write(fd, pattern, sizeof(pattern)) == (ssize_t)sizeof(pattern),
                "write pattern failed");
    TEST_ASSERT(fsync(fd) == 0, "fsync failed");
    close(fd);

    size_t n = 0;
    bool ingest_ok = khr_topology_ingest(&topo, path, &n);
    bool count_ok = ingest_ok && n == sizeof(pattern);
    bool data_ok = count_ok && memcmp(topo.hugepage, pattern, sizeof(pattern)) == 0;
    khr_topology_destroy(&topo);
    unlink(path);
    TEST_ASSERT(ingest_ok, "OPENAT2/READ_FIXED ingest failed");
    TEST_ASSERT(count_ok, "ingest byte count mismatch");
    TEST_ASSERT(data_ok, "hugepage does not contain ingested bytes");
    return true;
}

[[nodiscard]]
bool test_eventfd_starvation_watch(void) {
    khr_topology_t topo = {};
    TEST_ASSERT(khr_topology_init(&topo), "topology init failed");

    int efd = eventfd(0, EFD_CLOEXEC);
    if (efd < 0) {
        khr_topology_destroy(&topo);
        TEST_ASSERT(false, "eventfd failed");
    }
    bool armed = khr_topology_arm_eventfd(&topo, efd);
    uint64_t one = 1;
    bool wrote = armed && write(efd, &one, sizeof(one)) == (ssize_t)sizeof(one);
    struct io_uring_cqe cqe = {};
    bool got = wrote && khr_test_drain_cqe(&topo.ring_a, 1'000, &cqe);
    uint64_t ud = got ? cqe.user_data : 0;
    int res = got ? cqe.res : 0;
    khr_topology_destroy(&topo);
    close(efd);
    TEST_ASSERT(armed, "eventfd watch SQE failed");
    TEST_ASSERT(wrote, "eventfd write failed");
    TEST_ASSERT(got, "eventfd CQE missing");
    TEST_ASSERT(ud == KHR_TAG_EVENTFD, "eventfd user_data mismatch");
    TEST_ASSERT(res > 0, "eventfd watch did not fire");
    return true;
}

[[nodiscard]]
bool test_fixed_fd_install(void) {
    khr_uring_t ring = {};
    khr_uring_config_t cfg = khr_uring_config_ring_a();
    TEST_ASSERT(khr_uring_init(&ring, &cfg), "Ring A init failed");

    char path[] = "/tmp/khoros_fdin_XXXXXX";
    int fd = mkstemp(path);
    TEST_ASSERT(fd >= 0, "mkstemp failed");
    const char blob[] = "scm-rights";
    TEST_ASSERT(write(fd, blob, sizeof(blob) - 1) == (ssize_t)(sizeof(blob) - 1),
                "write failed");
    TEST_ASSERT(lseek(fd, 0, SEEK_SET) == 0, "lseek failed");

    int fds[1] = { fd };
    TEST_ASSERT(khr_uring_register_files(&ring, fds, 1), "IORING_REGISTER_FILES failed");

    TEST_ASSERT(khr_uring_prep_fixed_fd_install(&ring, 0, KHR_TAG_FD_INSTALL) != nullptr,
                "FIXED_FD_INSTALL SQE failed");
    struct io_uring_cqe cqe = {};
    TEST_ASSERT(khr_test_drain_cqe(&ring, 1'000, &cqe), "FIXED_FD_INSTALL CQE missing");
    TEST_ASSERT(cqe.res >= 0, "FIXED_FD_INSTALL failed");

    int installed = cqe.res;
    char got[16] = {};
    ssize_t n = read(installed, got, sizeof(got));
    TEST_ASSERT(n == (ssize_t)(sizeof(blob) - 1), "installed fd read length mismatch");
    TEST_ASSERT(memcmp(got, blob, sizeof(blob) - 1) == 0, "installed fd data mismatch");

    close(installed);
    close(fd);
    unlink(path);
    khr_uring_destroy(&ring);
    return true;
}

[[nodiscard]]
bool test_wayland_pbuf_inbound_outbound(void) {
    int sv[2] = { -1, -1 };
    TEST_ASSERT(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0,
                "socketpair failed");

    khr_uring_t ring = {};
    khr_uring_config_t cfg = khr_uring_config_ring_a();
    TEST_ASSERT(khr_uring_init(&ring, &cfg), "Ring A init failed");

    khr_pbuf_t t0 = {};
    TEST_ASSERT(khr_pbuf_init(&t0, &ring, KHR_PBUF_TIER0_BGID,
                              KHR_PBUF_TIER0_COUNT, KHR_PBUF_TIER0_SIZE),
                "tier-0 pbuf init failed");

    khr_wl_client_t client = {
        .sock_fd = sv[0],
        .ring = &ring,
    };
    khr_wl_client_attach_pbufs(&client, &t0, nullptr);
    TEST_ASSERT(khr_wl_client_arm_inbound(&client), "multishot inbound arm failed");

    const char wire[] = "wl-burst";
    TEST_ASSERT(send(sv[1], wire, sizeof(wire) - 1, 0) == (ssize_t)(sizeof(wire) - 1),
                "peer send failed");

    struct io_uring_cqe cqe = {};
    TEST_ASSERT(khr_test_drain_cqe(&ring, 1'000, &cqe), "inbound CQE missing");
    TEST_ASSERT(cqe.res >= 0, "inbound RECVMSG failed");
    TEST_ASSERT((cqe.flags & IORING_CQE_F_BUFFER) != 0, "inbound CQE missing buffer id");

    uint16_t bid = khr_cqe_buf_id(&cqe);
    khr_recvmsg_view_t view = {};
    TEST_ASSERT(khr_recvmsg_parse(khr_pbuf_data(&t0, bid), t0.buf_size, 0, 0, &view),
                "inbound recvmsg_out parse failed");
    TEST_ASSERT(view.payload_len == sizeof(wire) - 1, "inbound payload length mismatch");
    TEST_ASSERT(memcmp(view.payload, wire, sizeof(wire) - 1) == 0,
                "inbound payload mismatch");
    khr_pbuf_recycle(&t0, bid);

    const char out[] = "wl-out";
    TEST_ASSERT(khr_wl_client_send_skip(&client, out, sizeof(out) - 1),
                "outbound SENDMSG skip-success failed");
    char got[16] = {};
    ssize_t n = recv(sv[1], got, sizeof(got), 0);
    TEST_ASSERT(n == (ssize_t)(sizeof(out) - 1), "outbound peer recv length mismatch");
    TEST_ASSERT(memcmp(got, out, sizeof(out) - 1) == 0, "outbound payload mismatch");

    khr_pbuf_destroy(&t0);
    khr_uring_destroy(&ring);
    close(sv[0]);
    close(sv[1]);
    return true;
}

[[nodiscard]]
bool test_topology_cqe_staging_and_no_swallow(void) {
    khr_topology_t topo = {};
    TEST_ASSERT(khr_topology_init(&topo), "topology init failed");

    int sv[2] = { -1, -1 };
    TEST_ASSERT(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0, "socketpair failed");

    int efd = eventfd(0, EFD_CLOEXEC);
    TEST_ASSERT(efd >= 0, "eventfd failed");

    /* 1. Arm multishot RECVMSG on Ring A using PBUF tier 0 */
    struct msghdr rmsg = {};
    struct io_uring_sqe* rsqe = khr_uring_prep_recvmsg(&topo.ring_a, sv[0], &rmsg,
                                                      topo.pbuf_tier0.bgid, true,
                                                      KHR_TAG_RECVMSG);
    TEST_ASSERT_NOT_NULL(rsqe, "recvmsg prep failed");
    TEST_ASSERT(khr_uring_submit(&topo.ring_a, 0) >= 0, "recvmsg submit failed");

    /* 2. Arm eventfd watch on Ring A */
    TEST_ASSERT(khr_topology_arm_eventfd(&topo, efd), "arm eventfd failed");

    /* 3. Trigger eventfd and send socket data to generate 2 non-matching CQEs */
    uint64_t one = 1;
    TEST_ASSERT(write(efd, &one, sizeof(one)) == (ssize_t)sizeof(one), "eventfd write failed");

    const char pkt[] = "staged-pbuf-payload";
    TEST_ASSERT(send(sv[1], pkt, sizeof(pkt) - 1, 0) == (ssize_t)(sizeof(pkt) - 1), "send failed");

    /* Give brief pause for completions to land */
    for (uint32_t i = 0; i < 500; i++) {
        khr_cpu_pause();
    }

    /* 4. Now send BDA and wait for BDA.
     * khr_wait_tagged MUST harvest unexpected CQEs, stage them into topo.staged_cqes,
     * not leak the PBUF buffer, and successfully return the BDA completion. */
    constexpr uint64_t test_bda = 0xABCD'1234'DEAD'BEEFULL;
    TEST_ASSERT(khr_topology_signal_bda(&topo, test_bda), "signal BDA failed");

    uint64_t got_bda = 0;
    TEST_ASSERT(khr_topology_wait_bda(&topo, &got_bda, 2'000), "wait BDA failed");
    TEST_ASSERT_EQ(got_bda, test_bda, "BDA payload mismatch");

    /* 5. Verify that both non-matching CQEs were staged and NOT swallowed */
    size_t staged = khr_topology_staged_count(&topo);
    TEST_ASSERT_EQ(staged, (size_t)2, "expected exactly 2 staged CQEs");

    bool saw_efd = false;
    bool saw_pbuf = false;
    for (uint32_t i = 0; i < 2; i++) {
        khr_cqe_event_t evt = {};
        TEST_ASSERT(khr_topology_pop_cqe(&topo, &evt), "pop staged CQE failed");
        if (evt.user_data == KHR_TAG_EVENTFD) {
            saw_efd = true;
            TEST_ASSERT(evt.res > 0, "eventfd res must be positive");
        } else if (evt.user_data == KHR_TAG_RECVMSG) {
            saw_pbuf = true;
            TEST_ASSERT(evt.res >= 0, "recvmsg res failed");
            TEST_ASSERT((evt.flags & IORING_CQE_F_BUFFER) != 0, "recvmsg must have F_BUFFER");
            uint16_t bid = (uint16_t)(evt.flags >> IORING_CQE_BUFFER_SHIFT);
            uint8_t* bdata = khr_pbuf_data(&topo.pbuf_tier0, bid);
            TEST_ASSERT_NOT_NULL(bdata, "pbuf buffer pointer null");
            khr_recvmsg_view_t view = {};
            TEST_ASSERT(khr_recvmsg_parse(bdata, topo.pbuf_tier0.buf_size, 0, 0, &view), "parse recvmsg_out failed");
            TEST_ASSERT_EQ(view.payload_len, (uint32_t)(sizeof(pkt) - 1), "payload len mismatch");
            TEST_ASSERT(memcmp(view.payload, pkt, sizeof(pkt) - 1) == 0, "payload bytes mismatch");
            /* Recycle buffer back to tier0 */
            khr_topology_recycle_cqe_buffer(&topo, &evt);
        }
    }
    TEST_ASSERT(saw_efd, "eventfd CQE was swallowed");
    TEST_ASSERT(saw_pbuf, "pbuf recvmsg CQE was swallowed");
    TEST_ASSERT_EQ(khr_topology_staged_count(&topo), (size_t)0, "staged queue should be empty");

    khr_topology_destroy(&topo);
    close(sv[0]);
    close(sv[1]);
    close(efd);
    return true;
}

static bool helper_push_stack_path(khr_topology_t* topo, const char* src_path) {
    char stack_path[128] = {};
    strncpy(stack_path, src_path, sizeof(stack_path) - 1);
    stack_path[sizeof(stack_path) - 1] = '\0';
    size_t n = 0;
    bool ok = khr_topology_ingest(topo, stack_path, &n);
    /* Clobber stack frame before return to test pointer safety */
    memset(stack_path, 0xCC, sizeof(stack_path));
    return ok;
}

[[nodiscard]]
bool test_topology_stale_path_and_cmdq(void) {
    khr_topology_t topo = {};
    TEST_ASSERT(khr_topology_init(&topo), "topology init failed");

    char temp_path[] = "/tmp/khoros_stale_path_XXXXXX";
    int fd = mkstemp(temp_path);
    TEST_ASSERT(fd >= 0, "mkstemp failed");
    const char content[] = "stack-path-safety-data";
    TEST_ASSERT(write(fd, content, sizeof(content) - 1) == (ssize_t)(sizeof(content) - 1), "write failed");
    close(fd);

    /* Ingest via helper function where stack string is clobbered */
    bool ok = helper_push_stack_path(&topo, temp_path);
    unlink(temp_path);
    TEST_ASSERT(ok, "ingest with stack-allocated path failed");
    TEST_ASSERT(memcmp(topo.hugepage, content, sizeof(content) - 1) == 0, "hugepage data mismatch");

    /* Test path length boundary: path exceeding KHR_PATH_MAX - 1 rejected */
    char long_path[KHR_PATH_MAX + 10];
    memset(long_path, 'A', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';
    size_t dummy = 0;
    TEST_ASSERT(!khr_topology_ingest(&topo, long_path, &dummy), "oversized path must be rejected");

    /* Test null path rejected */
    TEST_ASSERT(!khr_topology_ingest(&topo, nullptr, &dummy), "null path must be rejected");

    khr_topology_destroy(&topo);
    return true;
}

[[nodiscard]]
bool test_topology_ingest_edge_cases(void) {
    khr_topology_t topo = {};
    TEST_ASSERT(khr_topology_init(&topo), "topology init failed");

    /* 1. Ingest non-existent file: must fail cleanly without crashing */
    size_t n = 0;
    bool bad_ok = khr_topology_ingest(&topo, "/tmp/khoros_nonexistent_12345678.tmp", &n);
    TEST_ASSERT(!bad_ok, "ingest of non-existent file must return false");

    /* 2. Ingest 0-byte file */
    char zero_path[] = "/tmp/khoros_zero_XXXXXX";
    int fd0 = mkstemp(zero_path);
    TEST_ASSERT(fd0 >= 0, "mkstemp failed");
    close(fd0);
    bool z_ok = khr_topology_ingest(&topo, zero_path, &n);
    unlink(zero_path);
    TEST_ASSERT(!z_ok, "0-byte file ingest must return false");

    /* 3. Ingest unaligned size (e.g. 777 bytes) */
    char unaligned_path[] = "/tmp/khoros_unaligned_XXXXXX";
    int fdu = mkstemp(unaligned_path);
    TEST_ASSERT(fdu >= 0, "mkstemp failed");
    uint8_t ubuf[777];
    for (uint32_t i = 0; i < sizeof(ubuf); i++) {
        ubuf[i] = (uint8_t)(i ^ 0x5A);
    }
    TEST_ASSERT(write(fdu, ubuf, sizeof(ubuf)) == (ssize_t)sizeof(ubuf), "write unaligned failed");
    close(fdu);

    size_t nu = 0;
    bool u_ok = khr_topology_ingest(&topo, unaligned_path, &nu);
    bool u_match = u_ok && nu == sizeof(ubuf) && memcmp(topo.hugepage, ubuf, sizeof(ubuf)) == 0;
    unlink(unaligned_path);
    TEST_ASSERT(u_ok, "unaligned ingest failed");
    TEST_ASSERT_EQ(nu, sizeof(ubuf), "unaligned byte count mismatch");
    TEST_ASSERT(u_match, "unaligned data mismatch");

    khr_topology_destroy(&topo);
    return true;
}

[[nodiscard]]
bool test_pbuf_multishot_cycle_and_exhaustion(void) {
    int sv[2] = { -1, -1 };
    TEST_ASSERT(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0, "socketpair failed");

    khr_uring_t ring = {};
    khr_uring_config_t cfg = khr_uring_config_ring_a();
    TEST_ASSERT(khr_uring_init(&ring, &cfg), "Ring A init failed");

    khr_pbuf_t pbuf = {};
    TEST_ASSERT(khr_pbuf_init(&pbuf, &ring, KHR_PBUF_TIER0_BGID,
                              KHR_PBUF_TIER0_COUNT, KHR_PBUF_TIER0_SIZE),
                "PBUF tier-0 registration failed");

    struct msghdr hdr = {};
    TEST_ASSERT_NOT_NULL(khr_uring_prep_recvmsg(&ring, sv[0], &hdr, pbuf.bgid, true, KHR_TAG_RECVMSG),
                         "recvmsg prep failed");
    TEST_ASSERT(khr_uring_submit(&ring, 0) >= 0, "submit failed");

    /* Send and recycle in 8 consecutive cycles */
    for (uint32_t cycle = 0; cycle < 8; cycle++) {
        char msg[32];
        int msg_len = snprintf(msg, sizeof(msg), "cycle-packet-%03u", cycle);
        TEST_ASSERT(send(sv[1], msg, msg_len, 0) == (ssize_t)msg_len, "send packet failed");

        struct io_uring_cqe cqe = {};
        TEST_ASSERT(khr_test_drain_cqe(&ring, 1'000, &cqe), "CQE missing in cycle");
        TEST_ASSERT(cqe.res >= 0, "recvmsg failed in cycle");
        TEST_ASSERT((cqe.flags & IORING_CQE_F_BUFFER) != 0, "CQE missing buffer flag");

        uint16_t bid = khr_cqe_buf_id(&cqe);
        uint8_t* bdata = khr_pbuf_data(&pbuf, bid);
        TEST_ASSERT_NOT_NULL(bdata, "bdata null");

        khr_recvmsg_view_t view = {};
        TEST_ASSERT(khr_recvmsg_parse(bdata, pbuf.buf_size, 0, 0, &view), "parse recvmsg_out failed");
        TEST_ASSERT_EQ(view.payload_len, (uint32_t)msg_len, "payload length mismatch");
        TEST_ASSERT(memcmp(view.payload, msg, msg_len) == 0, "payload content mismatch");

        /* Recycle buffer back into ring */
        khr_pbuf_recycle(&pbuf, bid);
    }

    /* Test out-of-bounds bid handling */
    TEST_ASSERT_NULL(khr_pbuf_data(&pbuf, (uint16_t)KHR_PBUF_TIER0_COUNT), "OOB bid must return nullptr");
    khr_pbuf_recycle(&pbuf, (uint16_t)KHR_PBUF_TIER0_COUNT); /* must not crash */

    /* Test recvmsg_out parsing with invalid/truncated buffer */
    khr_recvmsg_view_t bad_view = {};
    TEST_ASSERT(!khr_recvmsg_parse(nullptr, 100, 0, 0, &bad_view), "null buf must fail");
    uint8_t tiny[4] = {};
    TEST_ASSERT(!khr_recvmsg_parse(tiny, sizeof(tiny), 0, 0, &bad_view), "undersized cap must fail");

    khr_pbuf_destroy(&pbuf);
    khr_uring_destroy(&ring);
    close(sv[0]);
    close(sv[1]);
    return true;
}

[[nodiscard]]
bool test_ring_sq_saturation_and_edge_cases(void) {
    khr_uring_t ring = {};
    khr_uring_config_t cfg = khr_uring_config_ring_a();
    TEST_ASSERT(khr_uring_init(&ring, &cfg), "Ring A init failed");

    /* Fill SQ completely: cfg sq_entries = 64 */
    for (uint32_t i = 0; i < ring.sq_entries; i++) {
        struct io_uring_sqe* sqe = khr_uring_prep_nop(&ring, KHR_TAG_NOP);
        TEST_ASSERT_NOT_NULL(sqe, "SQE allocation within capacity failed");
    }

    /* Next allocation MUST fail with nullptr (SQ full) */
    struct io_uring_sqe* overflow_sqe = khr_uring_get_sqe(&ring);
    TEST_ASSERT_NULL(overflow_sqe, "allocation past SQ capacity must return nullptr");

    /* Submit and drain all 64 completions */
    int sub = khr_uring_submit(&ring, ring.sq_entries);
    TEST_ASSERT_EQ((uint32_t)sub, ring.sq_entries, "submitted count mismatch");

    for (uint32_t i = 0; i < ring.sq_entries; i++) {
        struct io_uring_cqe cqe = {};
        TEST_ASSERT(khr_test_drain_cqe(&ring, 1'000, &cqe), "drain CQE failed");
        TEST_ASSERT_EQ(cqe.user_data, KHR_TAG_NOP, "user_data mismatch");
    }

    /* Now SQ must be available again */
    struct io_uring_sqe* fresh_sqe = khr_uring_prep_nop(&ring, KHR_TAG_NOP);
    TEST_ASSERT_NOT_NULL(fresh_sqe, "SQE allocation after drain failed");
    TEST_ASSERT(khr_uring_submit(&ring, 1) >= 0, "submit after drain failed");
    struct io_uring_cqe cqe2 = {};
    TEST_ASSERT(khr_test_drain_cqe(&ring, 1'000, &cqe2), "drain after submit failed");

    khr_uring_destroy(&ring);
    return true;
}

[[nodiscard]]
bool test_pbuf_tier1_multishot_cycle_and_recycling(void) {
    int sv[2] = { -1, -1 };
    TEST_ASSERT(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0, "socketpair failed");

    khr_uring_t ring = {};
    khr_uring_config_t cfg = khr_uring_config_ring_a();
    TEST_ASSERT(khr_uring_init(&ring, &cfg), "Ring A init failed");

    khr_pbuf_t pbuf = {};
    TEST_ASSERT(khr_pbuf_init(&pbuf, &ring, KHR_PBUF_TIER1_BGID,
                              KHR_PBUF_TIER1_COUNT, KHR_PBUF_TIER1_SIZE),
                "PBUF tier-1 registration failed");

    struct msghdr hdr = {};
    TEST_ASSERT_NOT_NULL(khr_uring_prep_recvmsg(&ring, sv[0], &hdr, pbuf.bgid, true, KHR_TAG_RECVMSG_T1),
                         "recvmsg tier-1 prep failed");
    TEST_ASSERT(khr_uring_submit(&ring, 0) >= 0, "recvmsg tier-1 submit failed");

    constexpr size_t PAYLOAD_SIZE = 16'384; /* 16 KiB */
    uint8_t* pattern = (uint8_t*)malloc(PAYLOAD_SIZE);
    TEST_ASSERT_NOT_NULL(pattern, "malloc failed");

    for (uint32_t cycle = 0; cycle < 4; cycle++) {
        for (size_t j = 0; j < PAYLOAD_SIZE; j++) {
            pattern[j] = (uint8_t)(cycle ^ (j & 0xFF));
        }
        ssize_t sent = send(sv[1], pattern, PAYLOAD_SIZE, 0);
        TEST_ASSERT_EQ(sent, (ssize_t)PAYLOAD_SIZE, "send tier-1 packet failed");

        struct io_uring_cqe cqe = {};
        TEST_ASSERT(khr_test_drain_cqe(&ring, 1'000, &cqe), "tier-1 CQE missing");
        TEST_ASSERT_EQ(cqe.user_data, KHR_TAG_RECVMSG_T1, "tier-1 user_data mismatch");
        TEST_ASSERT(cqe.res >= 0, "tier-1 recvmsg failed");
        TEST_ASSERT((cqe.flags & IORING_CQE_F_BUFFER) != 0, "tier-1 CQE missing buffer flag");

        uint16_t bid = khr_cqe_buf_id(&cqe);
        TEST_ASSERT_LT(bid, (uint16_t)KHR_PBUF_TIER1_COUNT, "tier-1 bid out of bounds");

        uint8_t* bdata = khr_pbuf_data(&pbuf, bid);
        TEST_ASSERT_NOT_NULL(bdata, "tier-1 bdata nullptr");

        khr_recvmsg_view_t view = {};
        TEST_ASSERT(khr_recvmsg_parse(bdata, pbuf.buf_size, 0, 0, &view), "parse tier-1 recvmsg failed");
        TEST_ASSERT_EQ(view.payload_len, (uint32_t)PAYLOAD_SIZE, "tier-1 payload length mismatch");
        TEST_ASSERT_MEM_EQ(view.payload, pattern, PAYLOAD_SIZE, "tier-1 payload content mismatch");

        khr_pbuf_recycle(&pbuf, bid);
    }

    free(pattern);
    khr_pbuf_destroy(&pbuf);
    khr_uring_destroy(&ring);
    close(sv[0]);
    close(sv[1]);
    return true;
}

[[nodiscard]]
bool test_topology_cqe_deep_staging_saturation(void) {
    khr_topology_t topo = {};
    TEST_ASSERT(khr_topology_init(&topo), "topology init failed");

    int sv[2] = { -1, -1 };
    TEST_ASSERT(socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, sv) == 0, "socketpair failed");

    int efd = eventfd(0, EFD_CLOEXEC);
    TEST_ASSERT_GE(efd, 0, "eventfd creation failed");

    /* 1. Arm multishot RECVMSG on Ring A using PBUF tier 0 */
    struct msghdr rmsg = {};
    struct io_uring_sqe* rsqe = khr_uring_prep_recvmsg(&topo.ring_a, sv[0], &rmsg,
                                                      topo.pbuf_tier0.bgid, true,
                                                      KHR_TAG_RECVMSG);
    TEST_ASSERT_NOT_NULL(rsqe, "recvmsg prep failed");
    TEST_ASSERT_GE(khr_uring_submit(&topo.ring_a, 0), 0, "recvmsg submit failed");

    /* 2. Arm eventfd watch on Ring A */
    TEST_ASSERT(khr_topology_arm_eventfd(&topo, efd), "arm eventfd failed");

    /* 3. Flood datagram socket with 24 numbered packets + 1 eventfd trigger */
    constexpr uint32_t NUM_PKTS = 24;
    for (uint32_t i = 0; i < NUM_PKTS; i++) {
        char pkt[32];
        int plen = snprintf(pkt, sizeof(pkt), "flood-pkt-%03u", i);
        TEST_ASSERT_EQ(send(sv[1], pkt, (size_t)plen, 0), (ssize_t)plen, "send flood packet failed");
    }

    uint64_t one = 1;
    TEST_ASSERT_EQ(write(efd, &one, sizeof(one)), (ssize_t)sizeof(one), "eventfd write failed");

    for (uint32_t i = 0; i < 500; i++) {
        khr_cpu_pause();
    }

    /* 4. Now signal BDA and wait. khr_wait_tagged must harvest all 25 non-matching CQEs,
     * stage them safely without leaking any buffers, and return the BDA completion. */
    constexpr uint64_t test_bda = 0x8877'6655'4433'2211ULL;
    bool bda_sig_ok = khr_topology_signal_bda(&topo, test_bda);
    uint64_t got_bda = 0;
    bool bda_wait_ok = khr_topology_wait_bda(&topo, &got_bda, 3'000);
    bool bda_match = (got_bda == test_bda);

    /* 5. Check staged count and pop all events (from staged queue and Ring A) */
    size_t staged_count = khr_topology_staged_count(&topo);

    uint32_t socket_pkts_popped = 0;
    bool saw_eventfd = false;
    bool payload_mismatch = false;

    for (uint32_t i = 0; i < (NUM_PKTS + 1); i++) {
        khr_cqe_event_t evt = {};
        if (!khr_topology_wait_cqe(&topo, &evt, 1'000)) {
            break;
        }

        if (evt.user_data == KHR_TAG_EVENTFD) {
            saw_eventfd = true;
        } else if (evt.user_data == KHR_TAG_RECVMSG) {
            uint16_t bid = (uint16_t)(evt.flags >> IORING_CQE_BUFFER_SHIFT);
            uint8_t* bdata = khr_pbuf_data(&topo.pbuf_tier0, bid);
            if (bdata != nullptr) {
                khr_recvmsg_view_t view = {};
                if (khr_recvmsg_parse(bdata, topo.pbuf_tier0.buf_size, 0, 0, &view)) {
                    char expected[32];
                    int explen = snprintf(expected, sizeof(expected), "flood-pkt-%03u", socket_pkts_popped);
                    if (view.payload_len != (uint32_t)explen ||
                        memcmp(view.payload, expected, (size_t)explen) != 0) {
                        payload_mismatch = true;
                    }
                } else {
                    payload_mismatch = true;
                }
            } else {
                payload_mismatch = true;
            }
            khr_topology_recycle_cqe_buffer(&topo, &evt);
            socket_pkts_popped++;
        }
    }

    size_t remaining_staged = khr_topology_staged_count(&topo);

    /* Clean up topology and descriptors BEFORE asserting to guarantee worker is joined */
    khr_topology_destroy(&topo);
    close(sv[0]);
    close(sv[1]);
    close(efd);

    TEST_ASSERT(bda_sig_ok, "signal BDA failed");
    TEST_ASSERT(bda_wait_ok, "wait BDA failed");
    TEST_ASSERT(bda_match, "BDA payload mismatch");
    TEST_ASSERT_GT(staged_count, (size_t)0, "at least some non-matching CQEs must have been staged");
    TEST_ASSERT_LE(staged_count, (size_t)(NUM_PKTS + 1), "staged count within bounds");
    TEST_ASSERT(saw_eventfd, "eventfd notification was lost");
    TEST_ASSERT_EQ(socket_pkts_popped, NUM_PKTS, "all 24 packets must be popped in order");
    TEST_ASSERT(!payload_mismatch, "packet payload content/order mismatch");
    TEST_ASSERT_EQ(remaining_staged, (size_t)0, "staged queue must be empty");

    return true;
}

[[nodiscard]]
bool test_topology_bda_stress_and_cmdq_saturation(void) {
    /* 1. Multi-roundtrip BDA stress: 50 consecutive BDA signal-and-waits */
    khr_topology_t topo = {};
    TEST_ASSERT(khr_topology_init(&topo), "topology init failed");

    for (uint32_t i = 0; i < 50; i++) {
        uint64_t bda_val = 0xCAFE'BABE'0000'0000ULL + (uint64_t)i;
        TEST_ASSERT(khr_topology_signal_bda(&topo, bda_val), "signal BDA in stress loop failed");

        uint64_t got_bda = 0;
        TEST_ASSERT(khr_topology_wait_bda(&topo, &got_bda, 1'000), "wait BDA in stress loop failed");
        TEST_ASSERT_EQ(got_bda, bda_val, "bda payload mismatch in stress loop");
    }

    khr_topology_destroy(&topo);

    /* 2. Standalone CmdQ SPSC capacity and boundary test */
    khr_topology_t fake_topo = {};
    atomic_store_explicit(&fake_topo.cmdq_head, 0, memory_order_relaxed);
    atomic_store_explicit(&fake_topo.cmdq_tail, 0, memory_order_relaxed);

    /* Fill exactly KHR_CMDQ_CAP (8) items */
    for (uint32_t i = 0; i < KHR_CMDQ_CAP; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/tmp/cmdq_path_%u", i);
        TEST_ASSERT(khr_cmdq_push(&fake_topo, KHR_WCMD_MSG_BDA, (uint64_t)(100 + i), path),
                    "cmdq push within capacity failed");
    }

    /* 9th push MUST fail with false (queue full) */
    TEST_ASSERT(!khr_cmdq_push(&fake_topo, KHR_WCMD_INGEST, 999, "/tmp/overflow"),
                "cmdq push beyond capacity must return false");

    /* Pop all 8 items and verify FIFO order and deep-copied path string */
    for (uint32_t i = 0; i < KHR_CMDQ_CAP; i++) {
        khr_wcmd_item_t item = {};
        TEST_ASSERT(khr_cmdq_pop(&fake_topo, &item), "cmdq pop failed");
        TEST_ASSERT_EQ(item.cmd, (uint32_t)KHR_WCMD_MSG_BDA, "item cmd mismatch");
        TEST_ASSERT_EQ(item.u64, (uint64_t)(100 + i), "item u64 mismatch");

        char expected_path[32];
        snprintf(expected_path, sizeof(expected_path), "/tmp/cmdq_path_%u", i);
        TEST_ASSERT_EQ(strcmp(item.path, expected_path), 0, "item path string mismatch");
    }

    /* Pop on empty queue must return false */
    khr_wcmd_item_t empty_item = {};
    TEST_ASSERT(!khr_cmdq_pop(&fake_topo, &empty_item), "pop on empty queue must return false");

    /* Fresh push after drain must succeed */
    TEST_ASSERT(khr_cmdq_push(&fake_topo, KHR_WCMD_INGEST, 777, "/tmp/fresh"),
                "push after drain failed");
    khr_wcmd_item_t fresh = {};
    TEST_ASSERT(khr_cmdq_pop(&fake_topo, &fresh), "pop after fresh push failed");
    TEST_ASSERT_EQ(fresh.u64, 777ULL, "fresh u64 mismatch");

    return true;
}

[[nodiscard]]
bool test_topology_ingest_multiblock_large_file(void) {
    khr_topology_t topo = {};
    TEST_ASSERT(khr_topology_init(&topo), "topology init failed");

    /* 1. Large 64 KiB multi-block ingest */
    constexpr size_t FILE_SIZE = 65'536; /* 64 KiB */
    char large_path[] = "/tmp/khoros_multiblock_XXXXXX";
    int fd = mkstemp(large_path);
    TEST_ASSERT_GE(fd, 0, "mkstemp large file failed");

    uint8_t* pattern = (uint8_t*)malloc(FILE_SIZE);
    TEST_ASSERT_NOT_NULL(pattern, "malloc failed");

    for (size_t i = 0; i < FILE_SIZE; i++) {
        pattern[i] = (uint8_t)((i * 31U + 17U) & 0xFF);
    }
    TEST_ASSERT_EQ(write(fd, pattern, FILE_SIZE), (ssize_t)FILE_SIZE, "write large file failed");
    close(fd);

    size_t nread = 0;
    bool ok = khr_topology_ingest(&topo, large_path, &nread);
    unlink(large_path);

    TEST_ASSERT(ok, "large multi-block ingest failed");
    TEST_ASSERT_EQ(nread, FILE_SIZE, "ingested size mismatch");
    TEST_ASSERT_MEM_EQ(topo.hugepage, pattern, FILE_SIZE, "hugepage pattern mismatch across 64 KiB");
    free(pattern);

    /* 2. Boundary path length: exactly KHR_PATH_MAX - 1 (255 bytes) */
    char long_valid_path[KHR_PATH_MAX] = {};
    memset(long_valid_path, 'a', sizeof(long_valid_path) - 1);
    long_valid_path[sizeof(long_valid_path) - 1] = '\0';
    memcpy(long_valid_path, "/tmp/p_", 7);

    int lfd = open(long_valid_path, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (lfd >= 0) {
        const char msg[] = "boundary-path-data";
        (void)write(lfd, msg, sizeof(msg) - 1);
        close(lfd);

        size_t ln = 0;
        bool lok = khr_topology_ingest(&topo, long_valid_path, &ln);
        unlink(long_valid_path);
        TEST_ASSERT(lok, "ingest with KHR_PATH_MAX - 1 path failed");
        TEST_ASSERT_EQ(ln, sizeof(msg) - 1, "boundary path read size mismatch");
        TEST_ASSERT_MEM_EQ(topo.hugepage, msg, sizeof(msg) - 1, "boundary path data mismatch");
    }

    khr_topology_destroy(&topo);
    return true;
}

[[nodiscard]]
bool test_uring_timeout_precision_and_no_double_wait(void) {
    khr_uring_t ring = {};
    khr_uring_config_t cfg = khr_uring_config_ring_a();
    TEST_ASSERT(khr_uring_init(&ring, &cfg), "Ring A init failed");

    /* 1. Measure timeout precision on idle ring: must return false in ~25 ms, NOT 50+ ms */
    struct timespec t0 = {};
    clock_gettime(CLOCK_MONOTONIC, &t0);

    struct io_uring_cqe* cqe = nullptr;
    bool got = khr_uring_wait_cqe_timeout(&ring, &cqe, 25);
    TEST_ASSERT(!got, "wait on empty ring must return false");
    TEST_ASSERT_NULL(cqe, "cqe must be nullptr on timeout");

    struct timespec t1 = {};
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed_ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 +
                        (double)(t1.tv_nsec - t0.tv_nsec) / 1'000'000.0;

    /* Verify that elapsed time is bounded and does not double-wait */
    TEST_ASSERT_GE(elapsed_ms, 20.0, "timeout expired too early");
    TEST_ASSERT_LE(elapsed_ms, 65.0, "timeout took too long (potential double-wait fallthrough bug)");

    /* 2. Submit real 5 ms timeout SQE, wait with 500 ms timeout: must return true in ~5 ms */
    struct __kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 5'000'000 };
    TEST_ASSERT_NOT_NULL(khr_uring_prep_timeout(&ring, &ts, KHR_TAG_TIMEOUT), "timeout prep failed");

    clock_gettime(CLOCK_MONOTONIC, &t0);
    TEST_ASSERT(khr_uring_wait_cqe_timeout(&ring, &cqe, 500), "wait with arrival failed");
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double arrive_ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 +
                       (double)(t1.tv_nsec - t0.tv_nsec) / 1'000'000.0;

    TEST_ASSERT_LT(arrive_ms, 50.0, "arrival should take ~5 ms, well under 50 ms");
    TEST_ASSERT_NOT_NULL(cqe, "cqe must not be null");
    TEST_ASSERT_EQ(cqe->user_data, KHR_TAG_TIMEOUT, "user_data mismatch");
    TEST_ASSERT_EQ(cqe->res, -ETIME, "res must be -ETIME");
    khr_uring_cqe_seen(&ring, cqe);

    khr_uring_destroy(&ring);
    return true;
}

[[nodiscard]]
bool test_sparse_files_registration_and_update(void) {
    /* Initialize ring without registered files */
    khr_uring_config_t cfg = {
        .sq_entries = 16,
        .cq_entries = 32,
        .flags = IORING_SETUP_SINGLE_ISSUER |
                 IORING_SETUP_DEFER_TASKRUN |
                 IORING_SETUP_COOP_TASKRUN |
                 IORING_SETUP_NO_SQARRAY |
                 IORING_SETUP_CQSIZE,
        .register_ring_fd = false,
        .registered_files_nr = 0,
    };
    khr_uring_t ring = {};
    TEST_ASSERT(khr_uring_init(&ring, &cfg), "Ring init failed");
    TEST_ASSERT(!ring.files_registered, "Ring must initially have files_registered=false");
    TEST_ASSERT_EQ(ring.registered_files_count, 0U, "Initial registered_files_count must be 0");

    /* Register 16 sparse slots */
    TEST_ASSERT(khr_uring_register_files_sparse(&ring, 16), "Sparse file registration failed");
    TEST_ASSERT(ring.files_registered, "files_registered must be true after registration");
    TEST_ASSERT_EQ(ring.registered_files_count, 16U, "registered_files_count must be 16");

    /* Test openat2 into sparse slot 3 */
    char path[] = "/tmp/khr_sparse_test_XXXXXX";
    int fd = mkstemp(path);
    TEST_ASSERT(fd >= 0, "mkstemp failed");
    const char data[] = "sparse-slot-test-data";
    TEST_ASSERT(write(fd, data, sizeof(data)) == (ssize_t)sizeof(data), "write failed");
    close(fd);

    struct open_how how = { .flags = O_RDONLY };
    struct io_uring_sqe* sqe = khr_uring_prep_openat2(&ring, AT_FDCWD, path, &how, 3, true, 42);
    TEST_ASSERT_NOT_NULL(sqe, "prep_openat2 failed");
    TEST_ASSERT(khr_uring_submit(&ring, 1) >= 0, "submit failed");

    struct io_uring_cqe* cqe = nullptr;
    TEST_ASSERT(khr_uring_wait_cqe_timeout(&ring, &cqe, 1'000), "wait open cqe failed");
    TEST_ASSERT_NOT_NULL(cqe, "cqe must not be null");
    TEST_ASSERT_EQ(cqe->res, 0, "openat2 into direct slot 3 failed");
    khr_uring_cqe_seen(&ring, cqe);

    /* Close sparse slot 3 */
    struct io_uring_sqe* csqe = khr_uring_prep_close(&ring, 3, true, 43);
    TEST_ASSERT_NOT_NULL(csqe, "prep_close failed");
    TEST_ASSERT(khr_uring_submit(&ring, 1) >= 0, "submit close failed");
    TEST_ASSERT(khr_uring_wait_cqe_timeout(&ring, &cqe, 1'000), "wait close cqe failed");
    TEST_ASSERT_EQ(cqe->res, 0, "close direct slot 3 failed");
    khr_uring_cqe_seen(&ring, cqe);

    unlink(path);

    /* Test register files update: install active fd into sparse slot 5 */
    char up_path[] = "/tmp/khr_up_test_XXXXXX";
    int up_fd = mkstemp(up_path);
    TEST_ASSERT(up_fd >= 0, "mkstemp up_fd failed");
    const char up_data[] = "update-slot-5-payload";
    TEST_ASSERT(write(up_fd, up_data, sizeof(up_data)) == (ssize_t)sizeof(up_data), "write up_data failed");
    TEST_ASSERT(lseek(up_fd, 0, SEEK_SET) == 0, "lseek up_fd failed");

    int fds_to_update[1] = { up_fd };
    TEST_ASSERT(khr_uring_register_files_update(&ring, 5, fds_to_update, 1),
                "khr_uring_register_files_update failed");

    /* Direct read from sparse slot 5 using pure io_uring prep_read */
    char read_buf[64] = {};
    struct io_uring_sqe* rsqe = khr_uring_prep_read(&ring, 5, read_buf, (uint32_t)sizeof(up_data), 0, true, 44);
    TEST_ASSERT_NOT_NULL(rsqe, "prep_read direct failed");
    TEST_ASSERT(khr_uring_submit(&ring, 1) >= 0, "submit direct read failed");
    TEST_ASSERT(khr_uring_wait_cqe_timeout(&ring, &cqe, 1'000), "wait direct read cqe failed");
    TEST_ASSERT_EQ(cqe->res, (int)sizeof(up_data), "direct read length mismatch");
    TEST_ASSERT_MEM_EQ(read_buf, up_data, sizeof(up_data), "direct read payload mismatch");
    khr_uring_cqe_seen(&ring, cqe);

    /* Close local process fd; kernel retains reference in sparse slot 5 */
    close(up_fd);
    unlink(up_path);

    /* Clear sparse slot 5 using -1 via files update */
    int clear_fd[1] = { -1 };
    TEST_ASSERT(khr_uring_register_files_update(&ring, 5, clear_fd, 1),
                "clearing sparse slot 5 failed");

    /* Attempt direct read on cleared slot 5; must fail with -EBADF */
    rsqe = khr_uring_prep_read(&ring, 5, read_buf, sizeof(read_buf), 0, true, 45);
    TEST_ASSERT_NOT_NULL(rsqe, "prep_read on cleared slot failed");
    TEST_ASSERT(khr_uring_submit(&ring, 1) >= 0, "submit cleared read failed");
    TEST_ASSERT(khr_uring_wait_cqe_timeout(&ring, &cqe, 1'000), "wait cleared read cqe failed");
    TEST_ASSERT_EQ(cqe->res, -EBADF, "read on cleared direct slot must return -EBADF");
    khr_uring_cqe_seen(&ring, cqe);

    /* Unregister files */
    TEST_ASSERT(khr_uring_unregister_files(&ring), "unregister files failed");
    TEST_ASSERT(!ring.files_registered, "files_registered must be false after unregister");
    TEST_ASSERT_EQ(ring.registered_files_count, 0U, "registered_files_count must be 0");

    khr_uring_destroy(&ring);
    return true;
}

[[nodiscard]]
bool test_direct_descriptor_ingest_pipeline(void) {
    khr_uring_config_t cfg = khr_uring_config_ring_b();
    khr_uring_t ring = {};
    TEST_ASSERT(khr_uring_init(&ring, &cfg), "Ring B init failed");
    TEST_ASSERT(ring.files_registered, "Ring B must have registered files");

    /* Allocate and register 2MB buffer */
    size_t sz = 2 * 1024 * 1024;
    void* buf = mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    TEST_ASSERT(buf != MAP_FAILED, "mmap failed");
    struct iovec iov = { .iov_base = buf, .iov_len = sz };
    TEST_ASSERT(khr_uring_register_buffers(&ring, &iov, 1), "register buffers failed");

    /* Create test file */
    char path[] = "/tmp/khr_direct_ingest_XXXXXX";
    int fd = mkstemp(path);
    TEST_ASSERT(fd >= 0, "mkstemp failed");

    constexpr size_t TEST_LEN = 16'384;
    uint8_t src[16'384];
    for (size_t i = 0; i < TEST_LEN; i++) {
        src[i] = (uint8_t)(i ^ 0xC3);
    }
    TEST_ASSERT(write(fd, src, TEST_LEN) == (ssize_t)TEST_LEN, "write failed");
    close(fd);

    /* Exercise direct descriptor atomic ingest */
    size_t out_bytes = 0;
    int rc = khr_ingest_read_fixed(&ring, path, buf, 0, sz, &out_bytes, true);
    unlink(path);

    TEST_ASSERT_EQ(rc, 0, "ingest_read_fixed failed");
    TEST_ASSERT_EQ(out_bytes, TEST_LEN, "out_bytes mismatch");
    TEST_ASSERT_MEM_EQ(buf, src, TEST_LEN, "ingested data mismatch");

    /* Verify that direct descriptor slot never touched the POSIX file descriptor table */
    TEST_ASSERT_EQ(fcntl(500, F_GETFD), -1, "fd 500 must not exist in POSIX table");
    TEST_ASSERT_EQ(errno, EBADF, "errno must be EBADF");

    TEST_ASSERT(khr_uring_unregister_buffers(&ring), "unregister buffers failed");
    munmap(buf, sz);
    khr_uring_destroy(&ring);
    return true;
}

[[nodiscard]]
bool test_ingest_atomic_hardlink_failure_resilience(void) {
    khr_uring_config_t cfg = khr_uring_config_ring_b();
    khr_uring_t ring = {};
    TEST_ASSERT(khr_uring_init(&ring, &cfg), "Ring B init failed");

    size_t sz = 2 * 1024 * 1024;
    void* buf = mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    TEST_ASSERT(buf != MAP_FAILED, "mmap failed");
    struct iovec iov = { .iov_base = buf, .iov_len = sz };
    TEST_ASSERT(khr_uring_register_buffers(&ring, &iov, 1), "register buffers failed");

    /* 1. Missing file: must cleanly fail with -ENOENT and cancel linked SQEs without slot leak */
    size_t out_bytes = 0;
    int rc_nonexistent = khr_ingest_read_fixed(&ring, "/tmp/khr_nonexistent_file_xyz123.bin",
                                              buf, 0, sz, &out_bytes, true);
    TEST_ASSERT_EQ(rc_nonexistent, -ENOENT, "missing file must return -ENOENT");

    /* 2. Zero-byte file: must return 0 bytes read and release direct slot */
    char zero_path[] = "/tmp/khr_zero_ingest_XXXXXX";
    int zfd = mkstemp(zero_path);
    TEST_ASSERT(zfd >= 0, "mkstemp zero file failed");
    close(zfd);

    out_bytes = 12345;
    int rc_zero = khr_ingest_read_fixed(&ring, zero_path, buf, 0, sz, &out_bytes, true);
    unlink(zero_path);
    TEST_ASSERT_EQ(rc_zero, 0, "zero-byte file ingest must succeed with 0");
    TEST_ASSERT_EQ(out_bytes, 0U, "zero-byte file out_bytes must be 0");

    /* 3. Unaligned file with O_DIRECT fallback to non-O_DIRECT */
    char unaligned_path[] = "/tmp/khr_unaligned_XXXXXX";
    int ufd = mkstemp(unaligned_path);
    TEST_ASSERT(ufd >= 0, "mkstemp unaligned failed");
    constexpr size_t UNALIGNED_LEN = 1'234;
    uint8_t usrc[1'234];
    for (size_t i = 0; i < UNALIGNED_LEN; i++) {
        usrc[i] = (uint8_t)(i ^ 0x7E);
    }
    TEST_ASSERT(write(ufd, usrc, UNALIGNED_LEN) == (ssize_t)UNALIGNED_LEN, "write unaligned failed");
    close(ufd);

    out_bytes = 0;
    int rc_unaligned = khr_ingest_read_fixed(&ring, unaligned_path, buf, 0, sz, &out_bytes, true);
    unlink(unaligned_path);
    TEST_ASSERT_EQ(rc_unaligned, 0, "unaligned ingest must succeed via fallback");
    TEST_ASSERT_EQ(out_bytes, UNALIGNED_LEN, "unaligned out_bytes mismatch");
    TEST_ASSERT_MEM_EQ(buf, usrc, UNALIGNED_LEN, "unaligned ingested data mismatch");

    /* 4. Subsequent standard file immediately after: proves slot 0 is clean and uncorrupted */
    char final_path[] = "/tmp/khr_final_XXXXXX";
    int ffd = mkstemp(final_path);
    TEST_ASSERT(ffd >= 0, "mkstemp final failed");
    constexpr size_t FINAL_LEN = 4'096;
    uint8_t fsrc[4'096];
    for (size_t i = 0; i < FINAL_LEN; i++) {
        fsrc[i] = (uint8_t)(i * 37U);
    }
    TEST_ASSERT(write(ffd, fsrc, FINAL_LEN) == (ssize_t)FINAL_LEN, "write final failed");
    close(ffd);

    out_bytes = 0;
    int rc_final = khr_ingest_read_fixed(&ring, final_path, buf, 0, sz, &out_bytes, true);
    unlink(final_path);
    TEST_ASSERT_EQ(rc_final, 0, "final ingest after recovery must succeed");
    TEST_ASSERT_EQ(out_bytes, FINAL_LEN, "final out_bytes mismatch");
    TEST_ASSERT_MEM_EQ(buf, fsrc, FINAL_LEN, "final ingested data mismatch");

    TEST_ASSERT(khr_uring_unregister_buffers(&ring), "unregister buffers failed");
    munmap(buf, sz);
    khr_uring_destroy(&ring);
    return true;
}

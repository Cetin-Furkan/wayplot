#include "khoros/core/topology.h"
#include "khoros/core/cpu.h"
#include "khoros/uring/ingest.h"
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <sched.h>
#include <time.h>
#include <stdio.h>
#include <errno.h>

#ifndef MAP_HUGE_2MB
#define MAP_HUGE_2MB (21 << MAP_HUGE_SHIFT)
#endif

#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25
#endif

[[nodiscard]]
static bool khr_pin_cpu(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET((unsigned)cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
}

[[nodiscard]]
static void* khr_alloc_2mb(size_t* sz, bool* hugetlb, bool* collapsed) {
    *sz = KHR_HUGEPAGE_SZ;
    *hugetlb = false;
    *collapsed = false;
#ifndef __SANITIZE_ADDRESS__
    void* huge = mmap(nullptr, KHR_HUGEPAGE_SZ, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_2MB,
                      -1, 0);
    if (huge != MAP_FAILED) {
        *hugetlb = true;
        *collapsed = true;
        return huge;
    }
#endif
    void* p = mmap(nullptr, KHR_HUGEPAGE_SZ, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (p == MAP_FAILED) {
        return nullptr;
    }
    (void)madvise(p, KHR_HUGEPAGE_SZ, MADV_HUGEPAGE);
    if (madvise(p, KHR_HUGEPAGE_SZ, MADV_COLLAPSE) == 0) {
        *collapsed = true;
    }
    return p;
}

[[nodiscard]]
bool khr_cmdq_push(khr_topology_t* t, uint32_t cmd, uint64_t u64, const char* path) {
    uint32_t head = atomic_load_explicit(&t->cmdq_head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&t->cmdq_tail, memory_order_acquire);
    if ((head - tail) >= KHR_CMDQ_CAP) {
        return false;
    }
    khr_wcmd_item_t* slot = &t->cmdq[head & (KHR_CMDQ_CAP - 1U)];
    slot->cmd = cmd;
    slot->_pad = 0;
    slot->u64 = u64;
    if (path != nullptr) {
        size_t len = strnlen(path, sizeof(slot->path) - 1);
        memcpy(slot->path, path, len);
        slot->path[len] = '\0';
    } else {
        slot->path[0] = '\0';
    }
    atomic_store_explicit(&t->cmdq_head, head + 1U, memory_order_release);
    return true;
}

[[nodiscard]]
bool khr_cmdq_pop(khr_topology_t* t, khr_wcmd_item_t* out) {
    uint32_t tail = atomic_load_explicit(&t->cmdq_tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&t->cmdq_head, memory_order_acquire);
    if (head == tail) {
        return false;
    }
    *out = t->cmdq[tail & (KHR_CMDQ_CAP - 1U)];
    atomic_store_explicit(&t->cmdq_tail, tail + 1U, memory_order_release);
    return true;
}

[[nodiscard]]
bool khr_cmdq_peek(const khr_topology_t* t, khr_wcmd_item_t* out) {
    if (t == nullptr || out == nullptr) {
        return false;
    }
    uint32_t tail = atomic_load_explicit(&t->cmdq_tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&t->cmdq_head, memory_order_acquire);
    if (head == tail) {
        return false;
    }
    *out = t->cmdq[tail & (KHR_CMDQ_CAP - 1U)];
    return true;
}

[[nodiscard]]
static bool khr_worker_msg_ring(khr_topology_t* t, uint64_t payload, uint32_t res) {
    struct io_uring_sqe* sqe = khr_uring_prep_msg_ring(&t->ring_b, t->ring_a.ring_fd,
                                                       payload, res);
    if (sqe == nullptr) {
        return false;
    }
    int sub = khr_uring_submit(&t->ring_b, 0);
    return sub >= 0;
}

void khr_topology_recycle_cqe_buffer(khr_topology_t* topo, const khr_cqe_event_t* evt) {
    if (topo == nullptr || evt == nullptr || !topo->ring_a_live) {
        return;
    }
    if ((evt->flags & IORING_CQE_F_BUFFER) == 0) {
        return;
    }
    uint16_t bid = (uint16_t)(evt->flags >> IORING_CQE_BUFFER_SHIFT);

    /* Explicit tag-based routing for tier 1 vs tier 0 */
    if (evt->user_data == KHR_TAG_WL_RECV_T1 || evt->user_data == KHR_TAG_RECVMSG_T1) {
        if (topo->pbuf1_live && bid < topo->pbuf_tier1.entries) {
            khr_pbuf_recycle(&topo->pbuf_tier1, bid);
            return;
        }
    }
    if (evt->user_data == KHR_TAG_WL_RECV || evt->user_data == KHR_TAG_RECVMSG) {
        if (topo->pbuf0_live && bid < topo->pbuf_tier0.entries) {
            khr_pbuf_recycle(&topo->pbuf_tier0, bid);
            return;
        }
    }

    /* Untagged fallback: bids 0–15 are ambiguous (both tiers). Prefer not
     * recycling into the wrong ring over a silent leak of one buffer. */
    if (topo->pbuf0_live && bid >= topo->pbuf_tier1.entries &&
        bid < topo->pbuf_tier0.entries) {
        khr_pbuf_recycle(&topo->pbuf_tier0, bid);
    } else if (topo->pbuf1_live && bid >= topo->pbuf_tier0.entries &&
               bid < topo->pbuf_tier1.entries) {
        khr_pbuf_recycle(&topo->pbuf_tier1, bid);
    }
}

/* Single-threaded FIFO helpers (Core 0 pump queues). Power-of-two capacities
 * make indexing a mask op; no scans, no memmove, strictly O(1). */
static bool khr_evt_q_push(khr_cqe_event_t* q, uint32_t cap, uint32_t* head,
                           uint32_t* tail, const khr_cqe_event_t* evt) {
    if ((*head - *tail) >= cap) {
        return false;
    }
    q[*head & (cap - 1U)] = *evt;
    (*head)++;
    return true;
}

static bool khr_evt_q_pop(khr_cqe_event_t* q, uint32_t cap, uint32_t* head,
                          uint32_t* tail, khr_cqe_event_t* out) {
    if (*head == *tail) {
        return false;
    }
    *out = q[*tail & (cap - 1U)];
    (*tail)++;
    return true;
}

static void khr_pump_classify(khr_topology_t* topo, const khr_cqe_event_t* evt) {
    /* MSG_RING results carry their dispatch key in res (payload in user_data:
     * the kernel writes sqe->off into CQE user_data and sqe->len into res).
     * The outstanding gate narrows res-code aliasing to a genuinely
     * coincident in-flight signal plus an exact-size packet. */
    if (evt->res == (int32_t)KHR_MSG_RES_BDA && topo->bda_outstanding > 0) {
        if (!khr_evt_q_push(topo->bda_q, KHR_BDA_Q_CAP,
                            &topo->bda_head, &topo->bda_tail, evt)) {
            /* BDA stream overflow: keep the newest, drop it explicitly rather
             * than silently growing memory. The drop is counted by absence:
             * waiters pair every signal 1:1 in every shipped test. */
        }
        return;
    }
    if (evt->res == (int32_t)KHR_MSG_RES_INGEST && topo->ingest_outstanding > 0) {
        (void)khr_evt_q_push(topo->ingest_q, KHR_INGEST_Q_CAP,
                             &topo->ingest_head, &topo->ingest_tail, evt);
        return;
    }
    if (!khr_evt_q_push(topo->evt_q, KHR_EVT_Q_CAP,
                        &topo->evt_head, &topo->evt_tail, evt)) {
        /* Overflow: recycle the provided buffer immediately so the PBUF ring
         * never leaks, then drop the event (same contract as before). */
        khr_topology_recycle_cqe_buffer(topo, evt);
    }
}

uint32_t khr_topology_pump(khr_topology_t* topo, uint32_t timeout_ms) {
    if (topo == nullptr || !topo->ring_a_live) {
        return 0;
    }
    /* One kernel wait for the first completion, then a non-blocking drain of
     * everything the kernel has posted. A single wait serves every waiter. */
    struct io_uring_cqe* first = nullptr;
    if (!khr_uring_peek_cqe(&topo->ring_a, &first) || first == nullptr) {
        if (!khr_uring_wait_cqe_timeout(&topo->ring_a, &first, timeout_ms) ||
            first == nullptr) {
            return 0;
        }
    }
    uint32_t harvested = 0;
    for (;;) {
        struct io_uring_cqe* cqe = nullptr;
        if (!khr_uring_peek_cqe(&topo->ring_a, &cqe) || cqe == nullptr) {
            break;
        }
        khr_cqe_event_t evt = {
            .user_data = cqe->user_data,
            .res = cqe->res,
            .flags = cqe->flags,
        };
        khr_uring_cqe_seen(&topo->ring_a, cqe);
        khr_pump_classify(topo, &evt);
        harvested++;
    }
    return harvested;
}

[[nodiscard]]
bool khr_topology_await_tag(khr_topology_t* topo, uint64_t tag, int32_t* out_res,
                            uint32_t timeout_ms) {
    if (topo == nullptr || !topo->ring_a_live || out_res == nullptr) {
        return false;
    }
    struct timespec start = {};
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        struct timespec now = {};
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t elapsed_ms = ((now.tv_sec - start.tv_sec) * 1000LL) +
                             ((now.tv_nsec - start.tv_nsec) / 1'000'000LL);
        /* Scan first: a prior pump may already have classified our tag.
         * Pumping before the scan waits for a *new* CQE and can time out
         * with the send completion already sitting in evt_q. */
        for (uint32_t i = topo->evt_tail; i != topo->evt_head; i++) {
            uint32_t idx = i & (KHR_EVT_Q_CAP - 1U);
            if (topo->evt_q[idx].user_data == tag) {
                *out_res = topo->evt_q[idx].res;
                for (uint32_t j = i; j + 1U != topo->evt_head; j++) {
                    topo->evt_q[j & (KHR_EVT_Q_CAP - 1U)] =
                        topo->evt_q[(j + 1U) & (KHR_EVT_Q_CAP - 1U)];
                }
                topo->evt_head--;
                return true;
            }
        }
        if (elapsed_ms >= (int64_t)timeout_ms) {
            return false;
        }
        uint32_t remain = (uint32_t)((int64_t)timeout_ms - elapsed_ms);
        if (remain == 0) {
            remain = 1;
        }
        (void)khr_topology_pump(topo, remain);
    }
}

bool khr_topology_wake_worker(khr_topology_t* topo) {
    if (topo == nullptr || !topo->ring_a_live || !topo->worker_started) {
        return false;
    }
    int dst_fd = topo->ring_b.ring_fd;
    if (dst_fd < 0) {
        return false;
    }
    /* Submitted on Ring A (owned by this thread under SINGLE_ISSUER) into
     * Ring B. SKIP_SUCCESS: no CQE on the source side, one doorbell CQE
     * (res KHR_MSG_RES_WAKE) on the worker side. ring_b.ring_fd is stable:
     * written by the worker before worker_ready (release) and only torn
     * down after join. */
    struct io_uring_sqe* sqe = khr_uring_prep_msg_ring(&topo->ring_a, dst_fd,
                                                       0, KHR_MSG_RES_WAKE);
    if (sqe == nullptr) {
        return false;
    }
    return khr_uring_submit(&topo->ring_a, 0) >= 0;
}

/* Block until the chosen FIFO yields a payload or the deadline passes. Every
 * iteration performs at most one kernel wait; unrelated completions harvested
 * along the way are already classified into their own queues, so nothing is
 * ever swallowed and no waiter starves another. */
static bool khr_queue_wait(khr_topology_t* topo, khr_cqe_event_t* q, uint32_t cap,
                           uint32_t* head, uint32_t* tail, uint32_t* outstanding,
                           uint64_t* out_ud, uint32_t timeout_ms) {
    struct timespec start = {};
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        khr_cqe_event_t evt = {};
        if (khr_evt_q_pop(q, cap, head, tail, &evt)) {
            if (outstanding != nullptr && *outstanding > 0) {
                (*outstanding)--;
            }
            if (out_ud != nullptr) {
                *out_ud = evt.user_data;
            }
            return true;
        }
        struct timespec now = {};
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t elapsed_ms = ((now.tv_sec - start.tv_sec) * 1000LL) +
                             ((now.tv_nsec - start.tv_nsec) / 1'000'000LL);
        if (elapsed_ms >= (int64_t)timeout_ms) {
            /* Late CQE must not keep the res-code gate open (tier-1 packets
             * of size 0xBDA0/0x1E57 would otherwise classify as IPC). */
            if (outstanding != nullptr && *outstanding > 0) {
                (*outstanding)--;
            }
            return false;
        }
        uint32_t remain = timeout_ms - (uint32_t)elapsed_ms;
        if (remain == 0) {
            remain = 1;
        }
        if (khr_topology_pump(topo, remain) == 0) {
            if (outstanding != nullptr && *outstanding > 0) {
                (*outstanding)--;
            }
            return false;
        }
    }
}

/* Bounded kernel-parked wait for the worker. 50 ms backstop: wake doorbells
 * normally cut it short, but even a lost wake is recovered within one slice
 * without burning a core. */
constexpr uint32_t KHR_WORKER_PARK_MS = 50;

/* Harvest every available Ring B CQE without blocking. Wake doorbells are
 * consumed silently; ingest-chain tags advance the async op and, on CLOSE,
 * report the byte count to Core 0 with one MSG_RING. Returns true when an
 * ingest op completed on this pass. */
static bool khr_worker_drain_ring_b(khr_topology_t* t, khr_ingest_op_t* op) {
    bool ingest_done = false;
    for (;;) {
        struct io_uring_cqe* cqe = nullptr;
        if (!khr_uring_peek_cqe(&t->ring_b, &cqe) || cqe == nullptr) {
            break;
        }
        uint64_t tag = cqe->user_data;
        int res = cqe->res;
        khr_uring_cqe_seen(&t->ring_b, cqe);
        /* Doorbell needs BOTH halves: a chain CQE reports a byte count in
         * res, and a file of exactly 0x574B (22'347) bytes would otherwise
         * alias the wake code and be swallowed here. Wake doorbells always
         * carry payload 0, so tag == 0 disambiguates exactly. */
        if (res == (int32_t)KHR_MSG_RES_WAKE && tag == 0) {
            continue; /* Pure doorbell: the cmdq drain below does the work. */
        }
        if (op->active && khr_ingest_op_feed(op, tag, res)) {
            size_t n = 0;
            int fin = khr_ingest_op_finalize(op, &n);
            if (fin == -EINVAL) {
                /* O_DIRECT refused by the fs: one transparent resubmit plain. */
                khr_ingest_op_begin(op, false);
                if (khr_ingest_chain_submit(&t->ring_b, t->ingest_path,
                                            t->hugepage, 0, t->hugepage_sz,
                                            op) != 0) {
                    op->active = false;
                    (void)khr_worker_msg_ring(t, 0, KHR_MSG_RES_INGEST);
                    ingest_done = true;
                }
            } else {
                uint64_t payload = (fin == 0) ? (uint64_t)n : 0;
                op->active = false;
                t->ingest_active = false;
                (void)khr_worker_msg_ring(t, payload, KHR_MSG_RES_INGEST);
                ingest_done = true;
            }
        }
    }
    return ingest_done;
}

/* Start the async ingest op. Path bytes live in t->ingest_path until CLOSE.
 * Writes into the payload slice only (UI reserve at the high end is intact).
 * Returns false when the chain could not be submitted; the command stays
 * queued for the next harvest. */
static bool khr_worker_start_ingest(khr_topology_t* t, khr_ingest_op_t* op,
                                    const khr_wcmd_item_t* item) {
    size_t len = strnlen(item->path, sizeof(t->ingest_path) - 1);
    memcpy(t->ingest_path, item->path, len);
    t->ingest_path[len] = '\0';
    khr_ingest_op_begin(op, true);
    size_t cap = khr_hp_payload_cap(t->hugepage_sz);
    if (cap == 0 || khr_ingest_chain_submit(&t->ring_b, t->ingest_path,
                                           t->hugepage, 0, cap, op) != 0) {
        op->active = false;
        return false;
    }
    t->ingest_active = true;
    return true;
}

static void* khr_worker_main(void* arg) {
    khr_topology_t* t = (khr_topology_t*)arg;

    if (!khr_pin_cpu(t->compute_cpu)) {
        atomic_store_explicit(&t->worker_failed, true, memory_order_relaxed);
        atomic_store_explicit(&t->worker_ready, true, memory_order_release);
        return nullptr;
    }

    khr_uring_config_t cfg = khr_uring_config_ring_b();
    if (!khr_uring_init(&t->ring_b, &cfg)) {
        atomic_store_explicit(&t->worker_failed, true, memory_order_relaxed);
        atomic_store_explicit(&t->worker_ready, true, memory_order_release);
        return nullptr;
    }
    (void)khr_uring_register_iowq_aff(&t->ring_b, t->compute_cpu);

    struct iovec iov = {
        .iov_base = t->hugepage,
        .iov_len = t->hugepage_sz,
    };
    t->buffers_registered = khr_uring_register_buffers(&t->ring_b, &iov, 1);
    if (!t->buffers_registered) {
        khr_uring_destroy(&t->ring_b);
        atomic_store_explicit(&t->worker_failed, true, memory_order_relaxed);
        atomic_store_explicit(&t->worker_ready, true, memory_order_release);
        return nullptr;
    }

    t->compute_cpu_actual = sched_getcpu();
    atomic_store_explicit(&t->worker_failed, false, memory_order_relaxed);
    atomic_store_explicit(&t->worker_ready, true, memory_order_release);

    /* Store kernel: the worker owns Ring B exclusively (SINGLE_ISSUER) and
     * never spins. Each iteration harvests completions, drains every queued
     * command, then parks in the kernel until the next doorbell or DMA
     * completion. Ingest chains stay in flight across iterations. */
    khr_ingest_op_t op = {};
    for (;;) {
        (void)khr_worker_drain_ring_b(t, &op);

        khr_wcmd_item_t item = {};
        while (khr_cmdq_peek(t, &item)) {
            if (atomic_load_explicit(&t->stop, memory_order_acquire)) {
                break;
            }
            if (item.cmd == KHR_WCMD_INGEST) {
                if (op.active || !khr_worker_start_ingest(t, &op, &item)) {
                    break;
                }
                (void)khr_cmdq_pop(t, &item);
                continue;
            }
            (void)khr_cmdq_pop(t, &item);
            if (item.cmd == KHR_WCMD_MSG_BDA) {
                (void)khr_worker_msg_ring(t, item.u64, KHR_MSG_RES_BDA);
            }
        }

        if (atomic_load_explicit(&t->stop, memory_order_acquire) &&
            !op.active) {
            /* Peek left any unconsumed command queued; destroy() has
             * joined only after stop is set, so no new work can arrive. */
            break;
        }
        /* Park in the kernel. Doorbell MSG_RING, ingest CQEs, or the 50 ms
         * backstop ends the wait; PAUSE-spinning is gone. */
        struct io_uring_cqe* parked = nullptr;
        (void)khr_uring_wait_cqe_timeout(&t->ring_b, &parked, KHR_WORKER_PARK_MS);
    }

    (void)khr_uring_unregister_buffers(&t->ring_b);
    t->buffers_registered = false;
    khr_uring_destroy(&t->ring_b);
    return nullptr;
}

[[nodiscard]]
static bool khr_spin_ready(khr_topology_t* topo, uint32_t timeout_ms) {
    struct timespec start = {};
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (!atomic_load_explicit(&topo->worker_ready, memory_order_acquire)) {
        struct timespec now = {};
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t elapsed_ms = ((now.tv_sec - start.tv_sec) * 1000LL) +
                             ((now.tv_nsec - start.tv_nsec) / 1'000'000LL);
        if (elapsed_ms >= (int64_t)timeout_ms) {
            return false;
        }
        khr_cpu_pause();
    }
    return true;
}

[[nodiscard]]
bool khr_topology_init(khr_topology_t* topo) {
    if (topo == nullptr) {
        return false;
    }
    *topo = (khr_topology_t){};
    topo->present_cpu = KHR_CPU_PRESENT;
    topo->compute_cpu = KHR_CPU_COMPUTE;
    topo->present_cpu_actual = -1;
    topo->compute_cpu_actual = -1;

    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 2) {
        topo->compute_cpu = topo->present_cpu;
    }

    if (!khr_pin_cpu(topo->present_cpu)) {
        return false;
    }
    topo->present_cpu_actual = sched_getcpu();

    topo->std_page = mmap(nullptr, KHR_STD_PAGE_SZ, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (topo->std_page == MAP_FAILED) {
        topo->std_page = nullptr;
        khr_topology_destroy(topo);
        return false;
    }
    topo->std_page_sz = KHR_STD_PAGE_SZ;
    memset(topo->std_page, 0, KHR_STD_PAGE_SZ);

    topo->hugepage = khr_alloc_2mb(&topo->hugepage_sz, &topo->hugepage_hugetlb,
                                   &topo->hugepage_collapsed);
    if (topo->hugepage == nullptr) {
        khr_topology_destroy(topo);
        return false;
    }

    khr_uring_config_t cfg_a = khr_uring_config_ring_a();
    if (!khr_uring_init(&topo->ring_a, &cfg_a)) {
        khr_topology_destroy(topo);
        return false;
    }
    topo->ring_a_live = true;

    topo->clock_registered = khr_uring_register_clock(&topo->ring_a, CLOCK_MONOTONIC);
    khr_uring_enable_no_iowait(&topo->ring_a);
    topo->no_iowait = (topo->ring_a.enter_flags & IORING_ENTER_NO_IOWAIT) != 0;
    (void)khr_uring_register_iowq_aff(&topo->ring_a, topo->present_cpu);

    if (!khr_pbuf_init(&topo->pbuf_tier0, &topo->ring_a, KHR_PBUF_TIER0_BGID,
                       KHR_PBUF_TIER0_COUNT, KHR_PBUF_TIER0_SIZE)) {
        khr_topology_destroy(topo);
        return false;
    }
    topo->pbuf0_live = true;

    if (!khr_pbuf_init(&topo->pbuf_tier1, &topo->ring_a, KHR_PBUF_TIER1_BGID,
                       KHR_PBUF_TIER1_COUNT, KHR_PBUF_TIER1_SIZE)) {
        khr_topology_destroy(topo);
        return false;
    }
    topo->pbuf1_live = true;

    if (pthread_create(&topo->worker, nullptr, khr_worker_main, topo) != 0) {
        khr_topology_destroy(topo);
        return false;
    }
    topo->worker_started = true;

    if (!khr_spin_ready(topo, 2'000)) {
        khr_topology_destroy(topo);
        return false;
    }
    if (atomic_load_explicit(&topo->worker_failed, memory_order_acquire)) {
        khr_topology_destroy(topo);
        return false;
    }
    return true;
}

void khr_topology_destroy(khr_topology_t* topo) {
    if (topo == nullptr) {
        return;
    }
    if (topo->worker_started) {
        atomic_store_explicit(&topo->stop, true, memory_order_release);
        /* The worker parks in the kernel: doorbell it so join() returns
         * promptly instead of waiting out the 50 ms backstop. Best-effort;
         * an already-exited worker makes the submit fail harmlessly. */
        (void)khr_topology_wake_worker(topo);
        (void)pthread_join(topo->worker, nullptr);
        topo->worker_started = false;
    }
    /* Recycle any parked evt_q buffers before tearing down pbuf rings */
    for (uint32_t i = topo->evt_tail; i != topo->evt_head; i++) {
        khr_cqe_event_t evt = topo->evt_q[i & (KHR_EVT_Q_CAP - 1U)];
        if ((evt.flags & IORING_CQE_F_BUFFER) != 0) {
            khr_topology_recycle_cqe_buffer(topo, &evt);
        }
    }
    topo->evt_head = topo->evt_tail = 0;

    if (topo->pbuf1_live) {
        khr_pbuf_destroy(&topo->pbuf_tier1);
        topo->pbuf1_live = false;
    }
    if (topo->pbuf0_live) {
        khr_pbuf_destroy(&topo->pbuf_tier0);
        topo->pbuf0_live = false;
    }
    if (topo->ring_a_live) {
        khr_uring_destroy(&topo->ring_a);
        topo->ring_a_live = false;
    }
    if (topo->hugepage != nullptr) {
        munmap(topo->hugepage, topo->hugepage_sz);
        topo->hugepage = nullptr;
    }
    if (topo->std_page != nullptr) {
        munmap(topo->std_page, topo->std_page_sz);
        topo->std_page = nullptr;
    }
}

[[nodiscard]]
bool khr_topology_signal_bda(khr_topology_t* topo, uint64_t bda) {
    if (topo == nullptr || !topo->worker_started) {
        return false;
    }
    /* Enqueue, then doorbell: the worker parks in the kernel and only the
     * MSG_RING wake pulls it back out. The wake is best-effort; the 50 ms
     * park backstop covers a lost doorbell (wait_bda still pairs 1:1). */
    if (!khr_cmdq_push(topo, KHR_WCMD_MSG_BDA, bda, nullptr)) {
        return false;
    }
    topo->bda_outstanding++;
    (void)khr_topology_wake_worker(topo);
    return true;
}

[[nodiscard]]
bool khr_topology_wait_bda(khr_topology_t* topo, uint64_t* out_bda, uint32_t timeout_ms) {
    if (topo == nullptr || out_bda == nullptr) {
        return false;
    }
    return khr_queue_wait(topo, topo->bda_q, KHR_BDA_Q_CAP,
                          &topo->bda_head, &topo->bda_tail,
                          &topo->bda_outstanding, out_bda, timeout_ms);
}

[[nodiscard]]
bool khr_topology_ingest_submit(khr_topology_t* topo, const char* path) {
    if (topo == nullptr || path == nullptr || !topo->worker_started) {
        return false;
    }
    if (strnlen(path, KHR_PATH_MAX) >= KHR_PATH_MAX) {
        return false;
    }
    if (!khr_cmdq_push(topo, KHR_WCMD_INGEST, 0, path)) {
        return false;
    }
    topo->ingest_outstanding++;
    (void)khr_topology_wake_worker(topo);
    return true;
}

[[nodiscard]]
bool khr_topology_pop_ingest(khr_topology_t* topo, size_t* out_bytes) {
    if (topo == nullptr) {
        return false;
    }
    khr_cqe_event_t evt = {};
    if (!khr_evt_q_pop(topo->ingest_q, KHR_INGEST_Q_CAP,
                       &topo->ingest_head, &topo->ingest_tail, &evt)) {
        if (khr_topology_pump(topo, 0) == 0) {
            return false;
        }
        if (!khr_evt_q_pop(topo->ingest_q, KHR_INGEST_Q_CAP,
                           &topo->ingest_head, &topo->ingest_tail, &evt)) {
            return false;
        }
    }
    if (topo->ingest_outstanding > 0) {
        topo->ingest_outstanding--;
    }
    if (out_bytes != nullptr) {
        *out_bytes = (size_t)evt.user_data;
    }
    return evt.user_data > 0;
}

[[nodiscard]]
bool khr_topology_ingest(khr_topology_t* topo, const char* path, size_t* out_bytes) {
    if (!khr_topology_ingest_submit(topo, path)) {
        return false;
    }
    uint64_t n = 0;
    if (!khr_queue_wait(topo, topo->ingest_q, KHR_INGEST_Q_CAP,
                        &topo->ingest_head, &topo->ingest_tail,
                        &topo->ingest_outstanding, &n, 2'000)) {
        return false;
    }
    if (out_bytes != nullptr) {
        *out_bytes = (size_t)n;
    }
    return n > 0;
}

[[nodiscard]]
bool khr_topology_arm_eventfd(khr_topology_t* topo, int efd) {
    if (topo == nullptr || efd < 0 || !topo->ring_a_live) {
        return false;
    }
    struct io_uring_sqe* sqe = khr_uring_prep_eventfd_watch(&topo->ring_a, efd, KHR_TAG_EVENTFD);
    if (sqe == nullptr) {
        return false;
    }
    return khr_uring_submit(&topo->ring_a, 0) >= 0;
}

[[nodiscard]]
bool khr_topology_pop_cqe(khr_topology_t* topo, khr_cqe_event_t* out_evt) {
    if (topo == nullptr || out_evt == nullptr || !topo->ring_a_live) {
        return false;
    }
    /* Serve parked generic events first (FIFO, O(1)); else harvest the ring
     * directly through the pump so the observation is classified, not raw. */
    if (khr_evt_q_pop(topo->evt_q, KHR_EVT_Q_CAP,
                      &topo->evt_head, &topo->evt_tail, out_evt)) {
        return true;
    }
    if (khr_topology_pump(topo, 0) == 0) {
        return false;
    }
    return khr_evt_q_pop(topo->evt_q, KHR_EVT_Q_CAP,
                         &topo->evt_head, &topo->evt_tail, out_evt);
}

[[nodiscard]]
bool khr_topology_wait_cqe(khr_topology_t* topo, khr_cqe_event_t* out_evt, uint32_t timeout_ms) {
    if (topo == nullptr || out_evt == nullptr || !topo->ring_a_live) {
        return false;
    }
    if (khr_evt_q_pop(topo->evt_q, KHR_EVT_Q_CAP,
                      &topo->evt_head, &topo->evt_tail, out_evt)) {
        return true;
    }
    if (khr_topology_pump(topo, timeout_ms) == 0) {
        return false;
    }
    return khr_evt_q_pop(topo->evt_q, KHR_EVT_Q_CAP,
                         &topo->evt_head, &topo->evt_tail, out_evt);
}

[[nodiscard]]
size_t khr_topology_staged_count(const khr_topology_t* topo) {
    if (topo == nullptr) {
        return 0;
    }
    return (size_t)(topo->evt_head - topo->evt_tail);
}

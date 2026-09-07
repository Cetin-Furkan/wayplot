#ifndef KHOROS_CORE_TOPOLOGY_H
#define KHOROS_CORE_TOPOLOGY_H

#if __STDC_VERSION__ < 202311L
#error "Khoros requires pure ISO C23 (compile with -std=c23)"
#endif

#ifdef _STDBOOL_H
#error "<stdbool.h> is strictly banned. In C23, bool, true, and false are native language keywords."
#endif

#ifdef _STDALIGN_H
#error "<stdalign.h> is strictly banned. In C23, alignas and alignof are native language keywords."
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <pthread.h>
#include "khoros/core/attributes.h"
#include "khoros/uring/ring.h"
#include "khoros/uring/pbuf.h"

/* Presentation runs on core 2 and compute on core 3: cores 0/1 (and SMT
 * siblings 4/5) stay free for the kernel scheduler, IRQs, and the compositor,
 * so the engine never contends with the OS hot path. */
constexpr int      KHR_CPU_PRESENT     = 2;
constexpr int      KHR_CPU_COMPUTE     = 3;
constexpr size_t   KHR_HUGEPAGE_SZ     = 2'097'152; /* 2 MiB */
constexpr size_t   KHR_STD_PAGE_SZ     = 4'096;
constexpr uint32_t KHR_CMDQ_CAP        = 8; /* power of two, SPSC */
constexpr size_t   KHR_PATH_MAX        = 256;
/* Completion-pump FIFO capacities (all power of two for mask indexing).
 * BDA/INGEST queues carry 64-bit result payloads; EVT carries everything
 * else harvested from Ring A (Wayland PBUF packets, eventfd readiness). */
constexpr uint32_t KHR_BDA_Q_CAP       = 64;
constexpr uint32_t KHR_INGEST_Q_CAP    = 16;
constexpr uint32_t KHR_EVT_Q_CAP       = 256; /* Match KHR_RING_A_CQ_ENTRIES */

typedef enum {
    KHR_WCMD_IDLE = 0,
    KHR_WCMD_MSG_BDA,
    KHR_WCMD_INGEST,
} khr_wcmd_t;

typedef struct {
    uint64_t user_data;
    int32_t  res;
    uint32_t flags;
} khr_cqe_event_t;

typedef struct {
    uint32_t cmd;
    uint32_t _pad;
    uint64_t u64;
    char     path[KHR_PATH_MAX];
} khr_wcmd_item_t;

typedef struct {
    khr_uring_t ring_a;
    khr_uring_t ring_b;
    khr_pbuf_t  pbuf_tier0;
    khr_pbuf_t  pbuf_tier1;

    void*  hugepage;
    size_t hugepage_sz;
    bool   hugepage_hugetlb;
    bool   hugepage_collapsed;
    bool   buffers_registered;
    bool   clock_registered;
    bool   no_iowait;

    void*  std_page;
    size_t std_page_sz;

    int present_cpu;
    int compute_cpu;
    int present_cpu_actual;
    int compute_cpu_actual;

    pthread_t worker;
    bool worker_started;
    _Atomic bool worker_ready;
    _Atomic bool worker_failed;
    _Atomic bool stop;

    /* SPSC: Core 0 produces, Core 1 consumes. Pads keep head/tail
     * on distinct cache lines without forcing the whole struct to alignas(64)
     * (stack topology objects would otherwise misalign under UBSan).
     * A push alone never wakes the worker: every producer must follow it with
     * khr_topology_wake_worker(), which posts MSG_RING into Ring B so the
     * worker drops out of its kernel-parked wait instead of spinning. */
    _Atomic uint32_t cmdq_head;
    uint8_t          _cmdq_pad0[60];
    _Atomic uint32_t cmdq_tail;
    uint8_t          _cmdq_pad1[60];
    khr_wcmd_item_t  cmdq[KHR_CMDQ_CAP];

    /* Unified completion pump queues (Core 0 only: single-threaded FIFO
     * head/tail indices, O(1), no memmove). The pump harvests every available
     * Ring A CQE in one pass and classifies it: MSG_RING BDA/INGEST results
     * into their payload queues, everything else (Wayland multishot packets,
     * eventfd readiness) into evt_q. No waiter ever blocks on a tag while
     * unrelated completions pile up behind it. */
    khr_cqe_event_t  bda_q[KHR_BDA_Q_CAP];
    uint32_t         bda_head;
    uint32_t         bda_tail;
    khr_cqe_event_t  ingest_q[KHR_INGEST_Q_CAP];
    uint32_t         ingest_head;
    uint32_t         ingest_tail;
    khr_cqe_event_t  evt_q[KHR_EVT_Q_CAP];
    uint32_t         evt_head;
    uint32_t         evt_tail;

    /* Worker-owned async ingest operation (Core 1 only). The 3-SQE chain is
     * submitted and the worker returns to its parked wait; chain CQEs harvested
     * from Ring B advance the op, and completion is reported to Core 0 with a
     * single MSG_RING. ingest_path owns the path bytes until CLOSE lands. */
    bool     ingest_active;
    bool     ingest_odirect;
    char     ingest_path[KHR_PATH_MAX];
    int      ingest_open_res;
    int      ingest_read_res;
    int      ingest_close_res;
    uint32_t ingest_seen_mask;

    bool ring_a_live;
    bool pbuf0_live;
    bool pbuf1_live;
} khr_topology_t;

[[nodiscard]]
bool khr_topology_init(khr_topology_t* topo);

void khr_topology_destroy(khr_topology_t* topo);

[[nodiscard]]
bool khr_topology_signal_bda(khr_topology_t* topo, uint64_t bda);

[[nodiscard]]
bool khr_topology_wait_bda(khr_topology_t* topo, uint64_t* out_bda, uint32_t timeout_ms);

[[nodiscard]]
bool khr_topology_ingest(khr_topology_t* topo, const char* path, size_t* out_bytes);

[[nodiscard]]
bool khr_topology_arm_eventfd(khr_topology_t* topo, int efd);

[[nodiscard]]
bool khr_cmdq_push(khr_topology_t* t, uint32_t cmd, uint64_t u64, const char* path);

[[nodiscard]]
bool khr_cmdq_pop(khr_topology_t* t, khr_wcmd_item_t* out);

/*
 * Unified completion pump: issue a single kernel wait (at most timeout_ms),
 * then harvest every available Ring A CQE and classify it into the bda_q /
 * ingest_q / evt_q FIFOs. Returns the number of CQEs harvested on this call.
 * PBUF-backed events keep their buffer reservations: the consumer recycles
 * them with khr_topology_recycle_cqe_buffer() after processing. Only evt_q
 * overflow recycles immediately (nothing may leak the provided-buffer ring).
 */
uint32_t khr_topology_pump(khr_topology_t* topo, uint32_t timeout_ms);

/*
 * Wake the compute worker out of its kernel-parked Ring B wait. Best-effort:
 * on SQE exhaustion it returns false and the worker's bounded backstop wait
 * still picks the queued command up. Producers call this after every
 * khr_cmdq_push (signal_bda / ingest do it internally).
 */
bool khr_topology_wake_worker(khr_topology_t* topo);

[[nodiscard]]
bool khr_topology_pop_cqe(khr_topology_t* topo, khr_cqe_event_t* out_evt);

[[nodiscard]]
bool khr_topology_wait_cqe(khr_topology_t* topo, khr_cqe_event_t* out_evt, uint32_t timeout_ms);

/* Parked generic-event count (evt_q depth). Compat name for the old staging
 * counter: 0 means the pump has no unclaimed Wayland/eventfd completions. */
[[nodiscard]]
size_t khr_topology_staged_count(const khr_topology_t* topo);

void khr_topology_recycle_cqe_buffer(khr_topology_t* topo, const khr_cqe_event_t* evt);

#endif /* KHOROS_CORE_TOPOLOGY_H */

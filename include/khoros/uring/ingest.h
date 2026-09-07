#ifndef KHOROS_URING_INGEST_H
#define KHOROS_URING_INGEST_H

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
#include <linux/openat2.h>
#include "khoros/core/attributes.h"
#include "khoros/uring/ring.h"

/*
 * Hot-path ingest: OPENAT2 (O_DIRECT) then READ_FIXED into a buffer
 * previously registered with IORING_REGISTER_BUFFERS.
 * Must be issued from the ring's SINGLE_ISSUER thread.
 *
 * Two flavors share one chain builder:
 * - khr_ingest_read_fixed(): submit the atomic 3-SQE chain and reap it
 *   inline (bootstrap / test helper; the caller blocks).
 * - khr_ingest_chain_submit() + khr_ingest_op_feed() + khr_ingest_op_finalize():
 *   fully asynchronous. The owner submits, returns to its event loop, feeds
 *   each Ring CQE into the op as it arrives, and finalizes when feed reports
 *   completion. The worker thread uses this so disk DMA never parks Core 1.
 */
typedef struct {
    bool     active;
    bool     odirect;
    int      open_res;
    int      read_res;
    int      close_res;
    uint32_t seen_mask;
    /* Kernel keeps a pointer to this until the OPENAT2 SQE is issued.
     * Must outlive submit: the worker stores it on its long-lived op. */
    struct open_how how;
} khr_ingest_op_t;

constexpr uint32_t KHR_INGEST_SEEN_OPEN  = 1U << 0;
constexpr uint32_t KHR_INGEST_SEEN_READ  = 1U << 1;
constexpr uint32_t KHR_INGEST_SEEN_CLOSE = 1U << 2;
constexpr uint32_t KHR_INGEST_SEEN_ALL   =
    KHR_INGEST_SEEN_OPEN | KHR_INGEST_SEEN_READ | KHR_INGEST_SEEN_CLOSE;

[[nodiscard]]
int khr_ingest_read_fixed(khr_uring_t* ring, const char* path,
                          void* dst, uint16_t buf_index, size_t cap,
                          size_t* out_bytes, bool try_odirect);

/* Submit the 3-SQE linked chain, return immediately. 0 submitted,
 * negative -errno when nothing was queued (caller may retry later).
 * `op` owns `how` for the life of the chain — never a stack local of this
 * function. All three SQEs must be claimed before any is published; a
 * partial chain is converted to SKIP_SUCCESS NOPs and not issued. */
[[nodiscard]]
int khr_ingest_chain_submit(khr_uring_t* ring, const char* path,
                            void* dst, uint16_t buf_index, size_t cap,
                            khr_ingest_op_t* op);

void khr_ingest_op_begin(khr_ingest_op_t* op, bool odirect);

/* Feed one harvested CQE (tag + res). Returns true once the CLOSE completion
 * landed and the op is ready to finalize. Foreign tags are ignored. */
[[nodiscard]]
bool khr_ingest_op_feed(khr_ingest_op_t* op, uint64_t tag, int res);

/* Resolve a completed op: 0 with *out_bytes set, -EINVAL when the caller
 * must resubmit without O_DIRECT, otherwise negative -errno. */
[[nodiscard]]
int khr_ingest_op_finalize(const khr_ingest_op_t* op, size_t* out_bytes);

#endif /* KHOROS_URING_INGEST_H */

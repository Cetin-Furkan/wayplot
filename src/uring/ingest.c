#include "khoros/uring/ingest.h"
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <linux/openat2.h>

#ifdef _STDBOOL_H
#error "<stdbool.h> is strictly banned. In C23, bool, true, and false are native language keywords."
#endif

#ifdef _STDALIGN_H
#error "<stdalign.h> is strictly banned. In C23, alignas and alignof are native language keywords."
#endif

[[nodiscard]]
int khr_ingest_chain_submit(khr_uring_t* ring, const char* path,
                            void* dst, uint16_t buf_index, size_t cap,
                            bool try_odirect) {
    if (ring == nullptr || path == nullptr || dst == nullptr || cap == 0) {
        return -EINVAL;
    }
    /* Ensure ring has sparse file table registered for direct descriptor slot */
    if (!ring->files_registered) {
        if (!khr_uring_register_files_sparse(ring, KHR_DEFAULT_SPARSE_FILES)) {
            return -ENOMEM;
        }
    }

    constexpr uint32_t direct_slot = KHR_DIRECT_SLOT_INGEST;
    struct open_how how = {
        .flags = (uint64_t)(O_RDONLY | (try_odirect ? O_DIRECT : 0)),
        .mode = 0,
        .resolve = 0,
    };

    /*
     * Atomic Linked Ingest Pipeline:
     * SQE 0: OPENAT2 directly into fixed file slot direct_slot (IOSQE_IO_LINK)
     * SQE 1: READ_FIXED from direct_slot into registered hugepage buffer (IOSQE_IO_HARDLINK)
     * SQE 2: CLOSE direct_slot releasing the fixed file inside kernel.
     * HARDLINK guarantees CLOSE runs (and frees slot 0) even when READ_FIXED
     * fails on O_DIRECT alignment, so no userspace cleanup roundtrip is needed.
     */
    struct io_uring_sqe* sqe0 = khr_uring_prep_openat2(ring, AT_FDCWD, path, &how,
                                                       direct_slot, true,
                                                       KHR_TAG_INGEST_OPEN);
    if (sqe0 == nullptr) {
        return -EAGAIN;
    }
    sqe0->flags |= IOSQE_IO_LINK;

    struct io_uring_sqe* sqe1 = khr_uring_prep_read_fixed(ring, (int)direct_slot,
                                                          dst, (uint32_t)cap, 0,
                                                          buf_index, true,
                                                          KHR_TAG_INGEST_READ);
    if (sqe1 == nullptr) {
        sqe0->flags &= (uint8_t)~IOSQE_IO_LINK;
        (void)khr_uring_submit(ring, 0);
        return -EAGAIN;
    }
    sqe1->flags |= IOSQE_IO_HARDLINK;

    struct io_uring_sqe* sqe2 = khr_uring_prep_close(ring, (int)direct_slot,
                                                     true, KHR_TAG_INGEST_CLOSE);
    if (sqe2 == nullptr) {
        sqe1->flags &= (uint8_t)~IOSQE_IO_HARDLINK;
        (void)khr_uring_submit(ring, 0);
        return -EAGAIN;
    }

    /* Submit entire atomic chain in a single io_uring_enter and return:
     * completions arrive later and are reaped by the owner's event loop. */
    if (khr_uring_submit(ring, 0) < 0) {
        return -EIO;
    }
    return 0;
}

void khr_ingest_op_begin(khr_ingest_op_t* op, bool odirect) {
    if (op == nullptr) {
        return;
    }
    *op = (khr_ingest_op_t){
        .active = true,
        .odirect = odirect,
        .open_res = -ENODATA,
        .read_res = -ENODATA,
        .close_res = -ENODATA,
        .seen_mask = 0,
    };
}

[[nodiscard]]
bool khr_ingest_op_feed(khr_ingest_op_t* op, uint64_t tag, int res) {
    if (op == nullptr || !op->active) {
        return false;
    }
    if (tag == KHR_TAG_INGEST_OPEN) {
        op->open_res = res;
        op->seen_mask |= KHR_INGEST_SEEN_OPEN;
    } else if (tag == KHR_TAG_INGEST_READ) {
        op->read_res = res;
        op->seen_mask |= KHR_INGEST_SEEN_READ;
    } else if (tag == KHR_TAG_INGEST_CLOSE) {
        op->close_res = res;
        op->seen_mask |= KHR_INGEST_SEEN_CLOSE;
    } else {
        return false;
    }
    /* CLOSE always executes (HARDLINK) or posts -ECANCELED, so its arrival
     * marks the chain terminal regardless of the earlier results. */
    return (op->seen_mask & KHR_INGEST_SEEN_CLOSE) != 0;
}

[[nodiscard]]
int khr_ingest_op_finalize(const khr_ingest_op_t* op, size_t* out_bytes) {
    if (op == nullptr || !op->active) {
        return -EINVAL;
    }
    /* O_DIRECT rejected (unaligned fs such as tmpfs): caller resubmits plain. */
    if (op->odirect && (op->open_res == -EINVAL || op->read_res == -EINVAL)) {
        return -EINVAL;
    }
    if (op->open_res < 0) {
        return op->open_res;
    }
    if (op->read_res < 0) {
        return op->read_res;
    }
    if (out_bytes != nullptr) {
        *out_bytes = (size_t)op->read_res;
    }
    return 0;
}

[[nodiscard]]
static int khr_ingest_submit_chain(khr_uring_t* ring, const char* path,
                                   void* dst, uint16_t buf_index, size_t cap,
                                   bool try_odirect, size_t* out_bytes) {
    int sub = khr_ingest_chain_submit(ring, path, dst, buf_index, cap, try_odirect);
    if (sub != 0) {
        return (sub == -EINVAL) ? -EINVAL : -1;
    }

    khr_ingest_op_t op = {};
    khr_ingest_op_begin(&op, try_odirect);

    for (;;) {
        struct io_uring_cqe* cqe = nullptr;
        if (!khr_uring_wait_cqe_timeout(ring, &cqe, 2'000) || cqe == nullptr) {
            break;
        }
        uint64_t tag = cqe->user_data;
        int res = cqe->res;
        khr_uring_cqe_seen(ring, cqe);
        if (khr_ingest_op_feed(&op, tag, res)) {
            break;
        }
    }
    if ((op.seen_mask & KHR_INGEST_SEEN_CLOSE) == 0) {
        return -1;
    }

    /* Safety fallback: ensure direct slot is released if close did not execute */
    if (op.open_res >= 0 && op.close_res < 0) {
        struct io_uring_sqe* close_sqe = khr_uring_prep_close(ring, (int)KHR_DIRECT_SLOT_INGEST,
                                                             true, KHR_TAG_INGEST_CLOSE);
        if (close_sqe != nullptr) {
            if (khr_uring_submit(ring, 1) >= 0) {
                struct io_uring_cqe* ccqe = nullptr;
                if (khr_uring_wait_cqe_timeout(ring, &ccqe, 1'000) && ccqe != nullptr) {
                    khr_uring_cqe_seen(ring, ccqe);
                }
            }
        }
    }

    int fin = khr_ingest_op_finalize(&op, out_bytes);
    return (fin == -EINVAL) ? -EINVAL : ((fin == 0) ? 0 : fin);
}

[[nodiscard]]
int khr_ingest_read_fixed(khr_uring_t* ring, const char* path,
                          void* dst, uint16_t buf_index, size_t cap,
                          size_t* out_bytes, bool try_odirect) {
    if (ring == nullptr || path == nullptr || dst == nullptr || cap == 0) {
        return -1;
    }

    int ret = -1;
    if (try_odirect) {
        ret = khr_ingest_submit_chain(ring, path, dst, buf_index, cap, true, out_bytes);
        if (ret != -EINVAL) {
            return ret;
        }
    }

    return khr_ingest_submit_chain(ring, path, dst, buf_index, cap, false, out_bytes);
}

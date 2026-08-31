#ifndef URING_RING_H
#define URING_RING_H

#include <linux/io_uring.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct wp_uring {
    int fd;
    int enter_fd;
    unsigned enter_flags;
    int ring_reg_index;
    unsigned files_registered;
    struct io_uring_params params;

    void *sq_ptr;
    size_t sq_map_sz;
    void *sqes_ptr;
    size_t sqes_sz;

    _Atomic unsigned *sq_head;
    _Atomic unsigned *sq_tail;
    unsigned *sq_mask;
    unsigned *sq_entries;
    unsigned *sq_flags;
    unsigned *sq_array;

    _Atomic unsigned *cq_head;
    _Atomic unsigned *cq_tail;
    unsigned *cq_mask;
    unsigned *cq_entries;
    unsigned *cq_overflow;
    struct io_uring_cqe *cqes;

    unsigned sq_prepared;
    bool no_sqarray;
};

[[nodiscard]] int wp_uring_setup(struct wp_uring *r, unsigned sq_entries);
void wp_uring_destroy(struct wp_uring *r);
void wp_uring_log_caps(const struct wp_uring *r, FILE *out);

[[nodiscard]] int wp_uring_register(struct wp_uring *r, unsigned opcode,
                                    const void *arg, unsigned nr);
[[nodiscard]] int wp_uring_register_files(struct wp_uring *r, const int *fds, unsigned n);
[[nodiscard]] int wp_uring_register_ring_fd(struct wp_uring *r);

[[nodiscard]] struct io_uring_sqe *wp_uring_get_sqe(struct wp_uring *r);
[[nodiscard]] int wp_uring_submit(struct wp_uring *r, unsigned min_complete);
[[nodiscard]] int wp_uring_submit_timeout(struct wp_uring *r, unsigned min_complete,
                                          uint64_t timeout_ns);
[[nodiscard]] unsigned wp_uring_cq_ready(const struct wp_uring *r);
[[nodiscard]] struct io_uring_cqe *wp_uring_cqe_at(struct wp_uring *r, unsigned i);
void wp_uring_cq_advance(struct wp_uring *r, unsigned n);

#endif /* URING_RING_H */

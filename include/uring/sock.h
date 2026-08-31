#ifndef URING_SOCK_H
#define URING_SOCK_H

#include "uring/pbuf.h"

#include <stdint.h>
#include <sys/socket.h>

struct wp_recvmsg_view {
    const struct io_uring_recvmsg_out *out;
    const uint8_t *ctrl;
    const uint8_t *payload;
    uint16_t bid;
};

void wp_sqe_sendmsg(struct io_uring_sqe *sqe, int fd, const struct msghdr *msg,
                    uint64_t user_data, bool fixed_file);
void wp_sqe_recvmsg_multishot(struct io_uring_sqe *sqe, int fd, const struct msghdr *msg,
                              uint16_t bgid, uint64_t user_data, bool fixed_file);
void wp_sqe_poll_add(struct io_uring_sqe *sqe, int fd, unsigned events, uint64_t user_data);

[[nodiscard]] int wp_recvmsg_view(const struct wp_pbuf *p, const struct io_uring_cqe *cqe,
                                  const struct msghdr *posted, struct wp_recvmsg_view *v);
[[nodiscard]] int wp_cmsg_fds(const uint8_t *ctrl, size_t controllen, int *fds, int max_fds);

#endif /* URING_SOCK_H */

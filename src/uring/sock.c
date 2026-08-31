#define _GNU_SOURCE
#include "uring/sock.h"

#include <errno.h>
#include <string.h>

void wp_sqe_sendmsg(struct io_uring_sqe *sqe, int fd, const struct msghdr *msg,
                    uint64_t user_data, bool fixed_file)
{
    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_SENDMSG;
    sqe->fd = fd;
    sqe->addr = (uint64_t)(uintptr_t)msg;
    sqe->msg_flags = MSG_NOSIGNAL;
    sqe->user_data = user_data;
    if (fixed_file)
        sqe->flags |= IOSQE_FIXED_FILE;
}

void wp_sqe_recvmsg_multishot(struct io_uring_sqe *sqe, int fd, const struct msghdr *msg,
                              uint16_t bgid, uint64_t user_data, bool fixed_file)
{
    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_RECVMSG;
    sqe->flags = IOSQE_BUFFER_SELECT;
    sqe->ioprio = IORING_RECV_MULTISHOT | IORING_RECVSEND_POLL_FIRST;
    sqe->fd = fd;
    sqe->addr = (uint64_t)(uintptr_t)msg;
    sqe->msg_flags = MSG_CMSG_CLOEXEC;
    sqe->buf_group = bgid;
    sqe->user_data = user_data;
    if (fixed_file)
        sqe->flags |= IOSQE_FIXED_FILE;
}

void wp_sqe_poll_add(struct io_uring_sqe *sqe, int fd, unsigned events, uint64_t user_data)
{
    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_POLL_ADD;
    sqe->fd = fd;
    sqe->poll32_events = events;
    sqe->user_data = user_data;
}

int wp_recvmsg_view(const struct wp_pbuf *p, const struct io_uring_cqe *cqe,
                    const struct msghdr *posted, struct wp_recvmsg_view *v)
{
    uint16_t bid;
    uint8_t *raw;
    size_t hdr;
    size_t need;

    if (!p || !cqe || !posted || !v)
        return -EINVAL;
    if (cqe->res <= 0)
        return cqe->res < 0 ? cqe->res : -EIO;
    if (!(cqe->flags & IORING_CQE_F_BUFFER))
        return -EBADMSG;

    bid = (uint16_t)(cqe->flags >> IORING_CQE_BUFFER_SHIFT);
    raw = wp_pbuf_ptr(p, bid);
    if (!raw)
        return -ERANGE;

    hdr = sizeof(struct io_uring_recvmsg_out);
    need = hdr + (size_t)posted->msg_namelen + (size_t)posted->msg_controllen;
    if (need > p->buf_size)
        return -EMSGSIZE;

    memset(v, 0, sizeof(*v));
    v->bid = bid;
    v->out = (const struct io_uring_recvmsg_out *)raw;
    v->ctrl = raw + hdr + posted->msg_namelen;
    v->payload = v->ctrl + posted->msg_controllen;
    return 0;
}

int wp_cmsg_fds(const uint8_t *ctrl, size_t controllen, int *fds, int max_fds)
{
    int count = 0;
    const uint8_t *end;
    const struct cmsghdr *cmsg;

    if (!ctrl || !fds || max_fds <= 0 || controllen < sizeof(struct cmsghdr))
        return 0;

    end = ctrl + controllen;
    cmsg = (const struct cmsghdr *)ctrl;
    while ((const uint8_t *)(cmsg + 1) <= end) {
        size_t align_len;

        if (cmsg->cmsg_len < sizeof(struct cmsghdr) ||
            (const uint8_t *)cmsg + cmsg->cmsg_len > end)
            break;
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            size_t data_len = cmsg->cmsg_len - CMSG_LEN(0);
            int n = (int)(data_len / sizeof(int));
            const int *in = (const int *)CMSG_DATA(cmsg);
            for (int i = 0; i < n && count < max_fds; i++)
                fds[count++] = in[i];
        }
        align_len = CMSG_ALIGN(cmsg->cmsg_len);
        if (align_len == 0 || (const uint8_t *)cmsg + align_len > end)
            break;
        cmsg = (const struct cmsghdr *)((const uint8_t *)cmsg + align_len);
    }
    return count;
}

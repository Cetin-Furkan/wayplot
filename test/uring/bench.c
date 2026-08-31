#define _GNU_SOURCE
#include "uring/pbuf.h"
#include "uring/ring.h"
#include "uring/sock.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static uint64_t nsec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int arm_recv(struct wp_uring *r, int rx, const struct msghdr *tmpl, uint16_t bgid)
{
    struct io_uring_sqe *sqe = wp_uring_get_sqe(r);
    if (!sqe)
        return -ENOSPC;
    wp_sqe_recvmsg_multishot(sqe, rx, tmpl, bgid, 1, true);
    return wp_uring_submit(r, 0);
}

static int bench_one(struct wp_uring *r, struct wp_pbuf *pbuf, int tx, int rx,
                     size_t msg_len, unsigned nmsg, uint16_t bgid)
{
    enum { SLOTS = 32 };
    struct msghdr recv_tmpl;
    char recv_cmsg[CMSG_SPACE(sizeof(int) * 4)];
    uint8_t *payload;
    struct iovec iov[SLOTS];
    struct msghdr smsg[SLOTS];
    uint8_t busy[SLOTS];
    unsigned inflight = 0, sent = 0, recv_armed = 0, max_inf;
    uint64_t got = 0, want, t0, t1, dt, deadline;
    int ret;

    want = (uint64_t)msg_len * nmsg;
    payload = malloc(msg_len);
    if (!payload)
        return -ENOMEM;
    memset(payload, 0x5a, msg_len);
    memset(busy, 0, sizeof(busy));
    memset(&recv_tmpl, 0, sizeof(recv_tmpl));
    recv_tmpl.msg_control = recv_cmsg;
    recv_tmpl.msg_controllen = sizeof(recv_cmsg);

    ret = arm_recv(r, rx, &recv_tmpl, bgid);
    if (ret < 0) {
        free(payload);
        return ret;
    }
    recv_armed = 1;
    max_inf = SLOTS;
    if (msg_len > 4096)
        max_inf = 4;
    if (msg_len > 16384)
        max_inf = 2;

    t0 = nsec();
    deadline = t0 + 10000000000ull;

    while (got < want) {
        unsigned s;

        if (nsec() > deadline) {
            ret = -ETIMEDOUT;
            goto out;
        }

        for (s = 0; s < SLOTS && sent < nmsg && inflight < max_inf; s++) {
            struct io_uring_sqe *sqe;
            if (busy[s])
                continue;
            sqe = wp_uring_get_sqe(r);
            if (!sqe)
                break;
            memset(&smsg[s], 0, sizeof(smsg[s]));
            iov[s].iov_base = payload;
            iov[s].iov_len = msg_len;
            smsg[s].msg_iov = &iov[s];
            smsg[s].msg_iovlen = 1;
            wp_sqe_sendmsg(sqe, tx, &smsg[s], 0x1000ull + s, true);
            busy[s] = 1;
            inflight++;
            sent++;
        }

        if (!recv_armed) {
            ret = arm_recv(r, rx, &recv_tmpl, bgid);
            if (ret < 0)
                goto out;
            recv_armed = 1;
        }

        ret = wp_uring_submit(r, 0);
        if (ret < 0)
            goto out;

        {
            unsigned ready = wp_uring_cq_ready(r);
            unsigned k;
            if (ready == 0)
                continue;
            for (k = 0; k < ready; k++) {
                struct io_uring_cqe *cqe = wp_uring_cqe_at(r, k);
                if (cqe->user_data == 1) {
                    if (cqe->res == -ENOBUFS) {
                        recv_armed = 0;
                    } else if (cqe->res > 0) {
                        struct wp_recvmsg_view v;
                        if (wp_recvmsg_view(pbuf, cqe, &recv_tmpl, &v) == 0) {
                            got += v.out->payloadlen;
                            wp_pbuf_recycle(pbuf, v.bid);
                        }
                        if (!(cqe->flags & IORING_CQE_F_MORE))
                            recv_armed = 0;
                    } else {
                        ret = cqe->res ? cqe->res : -EPIPE;
                        wp_uring_cq_advance(r, ready);
                        goto out;
                    }
                } else if (cqe->user_data >= 0x1000ull &&
                           cqe->user_data < 0x1000ull + SLOTS) {
                    unsigned si = (unsigned)(cqe->user_data - 0x1000ull);
                    busy[si] = 0;
                    inflight--;
                    if (cqe->res < 0) {
                        ret = cqe->res;
                        wp_uring_cq_advance(r, ready);
                        goto out;
                    }
                }
            }
            wp_uring_cq_advance(r, ready);
        }
    }
    t1 = nsec();
    dt = t1 > t0 ? t1 - t0 : 1;
    {
        double sec = (double)dt / 1e9;
        printf("  %7zu B  x %7u  %8.0f msg/s  %7.1f MiB/s  %6.2f ms\n",
               msg_len, nmsg, (double)nmsg / sec,
               ((double)want / (1024.0 * 1024.0)) / sec, sec * 1000.0);
    }
    ret = 0;
out:
    {
        struct io_uring_sqe *sqe = wp_uring_get_sqe(r);
        if (sqe) {
            sqe->opcode = IORING_OP_ASYNC_CANCEL;
            sqe->addr = 1;
            sqe->user_data = 0xdead;
            (void)wp_uring_submit(r, 0);
        }
        while (wp_uring_cq_ready(r) || inflight) {
            unsigned ready;
            if (wp_uring_submit(r, 0) < 0)
                break;
            if (nsec() > deadline + 1000000000ull)
                break;
            ready = wp_uring_cq_ready(r);
            if (!ready)
                break;
            for (unsigned k = 0; k < ready; k++) {
                struct io_uring_cqe *c = wp_uring_cqe_at(r, k);
                if (c->user_data >= 0x1000ull && c->user_data < 0x1000ull + SLOTS)
                    inflight--;
                if (c->user_data == 1 && (c->flags & IORING_CQE_F_BUFFER)) {
                    uint16_t bid = (uint16_t)(c->flags >> IORING_CQE_BUFFER_SHIFT);
                    wp_pbuf_recycle(pbuf, bid);
                }
            }
            wp_uring_cq_advance(r, ready);
        }
    }
    free(payload);
    return ret;
}

int main(void)
{
    struct wp_uring ring;
    struct wp_pbuf pbuf;
    int socks[2];
    int rc = 0;
    const struct {
        size_t len;
        unsigned n;
    } cases[] = {
        { 8,     100000 },
        { 16,    100000 },
        { 32,    100000 },
        { 128,    50000 },
        { 1024,   20000 },
        { 4096,    8000 },
        { 8000,    4000 },
        { 65532,    200 },
    };

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("wayland-sized unix stream bench  (pbuf 64 x 8192)\n");
    printf("  payload     count      rate         bandwidth   wall\n");

    if (wp_uring_setup(&ring, 128) < 0)
        return 1;
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, socks) < 0)
        return 1;
    if (wp_uring_register_files(&ring, socks, 2) < 0)
        return 1;
    if (wp_pbuf_setup(&ring, &pbuf, 64, 8192, 1) < 0)
        return 1;

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int ret = bench_one(&ring, &pbuf, 0, 1, cases[i].len, cases[i].n, 1);
        if (ret < 0) {
            printf("FAIL  size %zu (%s)\n", cases[i].len, strerror(-ret));
            rc = 1;
            break;
        }
    }

    wp_pbuf_destroy(&ring, &pbuf);
    wp_uring_destroy(&ring);
    close(socks[0]);
    close(socks[1]);
    return rc;
}

#define _GNU_SOURCE
#include "uring/pbuf.h"
#include "uring/ring.h"
#include "uring/sock.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

static int g_fail;

static void expect(int cond, const char *what)
{
    if (cond) {
        printf("PASS  %s\n", what);
        return;
    }
    printf("FAIL  %s\n", what);
    g_fail++;
}

static uint64_t nsec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int wait_n(struct wp_uring *r, unsigned need)
{
    int spins = 0;

    while (wp_uring_cq_ready(r) < need) {
        int ret = wp_uring_submit(r, 1);
        if (ret < 0)
            return ret;
        if (++spins > 1000000)
            return -ETIMEDOUT;
    }
    return 0;
}

static int test_nop(struct wp_uring *r)
{
    struct io_uring_sqe *sqe;
    struct io_uring_cqe *cqe;
    uint64_t t0, t1;
    int ret;

    sqe = wp_uring_get_sqe(r);
    if (!sqe)
        return -ENOSPC;
    sqe->opcode = IORING_OP_NOP;
    sqe->user_data = 0x4e4f50ull;
    t0 = nsec();
    ret = wp_uring_submit(r, 1);
    if (ret < 0)
        return ret;
    ret = wait_n(r, 1);
    t1 = nsec();
    if (ret < 0)
        return ret;
    cqe = wp_uring_cqe_at(r, 0);
    expect(cqe->user_data == 0x4e4f50ull && cqe->res == 0, "NOP completion");
    printf("      nop round-trip %llu ns\n", (unsigned long long)(t1 - t0));
    wp_uring_cq_advance(r, 1);
    return 0;
}

static int test_timeout(struct wp_uring *r)
{
    struct io_uring_sqe *sqe;
    struct io_uring_cqe *cqe;
    struct __kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 5 * 1000 * 1000 };
    int ret;

    sqe = wp_uring_get_sqe(r);
    if (!sqe)
        return -ENOSPC;
    sqe->opcode = IORING_OP_TIMEOUT;
    sqe->addr = (uint64_t)(uintptr_t)&ts;
    sqe->len = 1;
    sqe->user_data = 0x54494d45ull;
    ret = wp_uring_submit(r, 1);
    if (ret < 0)
        return ret;
    ret = wait_n(r, 1);
    if (ret < 0)
        return ret;
    cqe = wp_uring_cqe_at(r, 0);
    expect(cqe->user_data == 0x54494d45ull && cqe->res == -ETIME, "TIMEOUT fires -ETIME");
    if (cqe->res != -ETIME)
        printf("      timeout res=%d (%s)\n", cqe->res,
               cqe->res < 0 ? strerror(-cqe->res) : "");
    wp_uring_cq_advance(r, 1);
    return 0;
}

static int test_multishot_rights(struct wp_uring *r)
{
    enum { NMSG = 3, BGID = 1, BSZ = 4096, NBUF = 16 };
    int socks[2] = {-1, -1};
    int pipefd[2] = {-1, -1};
    int got_fd = -1;
    struct wp_pbuf pbuf;
    struct msghdr recv_tmpl;
    char recv_cmsg[CMSG_SPACE(sizeof(int) * 4)];
    char payloads[NMSG][32];
    struct iovec iov[NMSG];
    struct msghdr smsg[NMSG];
    char cmsgbuf[CMSG_SPACE(sizeof(int))];
    unsigned nrecv = 0, nsend = 0, nmore = 0, nrights = 0;
    int ret;
    unsigned ready, i;

    memset(&pbuf, 0, sizeof(pbuf));
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, socks) < 0)
        return -errno;
    if (pipe2(pipefd, O_CLOEXEC) < 0) {
        ret = -errno;
        goto close_socks;
    }

    ret = wp_uring_register_files(r, socks, 2);
    expect(ret == 0, "REGISTER_FILES (2 unix fds)");
    if (ret < 0) {
        printf("      register_files %s\n", strerror(-ret));
        goto close_pipe;
    }

    ret = wp_pbuf_setup(r, &pbuf, NBUF, BSZ, BGID);
    expect(ret == 0, "REGISTER_PBUF_RING");
    if (ret < 0) {
        printf("      pbuf %s\n", strerror(-ret));
        goto close_pipe;
    }

    {
        struct io_uring_buf_reg inc = { 0 };
        struct wp_pbuf dummy;
        void *mem = mmap(NULL, sizeof(struct io_uring_buf) * 8,
                         PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        int inc_ok = 0;
        if (mem != MAP_FAILED) {
            inc.ring_addr = (uint64_t)(uintptr_t)mem;
            inc.ring_entries = 8;
            inc.bgid = 2;
            inc.flags = IOU_PBUF_RING_INC;
            inc_ok = wp_uring_register(r, IORING_REGISTER_PBUF_RING, &inc, 1) == 0;
            if (inc_ok) {
                inc.ring_addr = 0;
                (void)wp_uring_register(r, IORING_UNREGISTER_PBUF_RING, &inc, 1);
            }
            munmap(mem, sizeof(struct io_uring_buf) * 8);
        }
        expect(inc_ok, "IOU_PBUF_RING_INC can register (not used for recvmsg)");
        (void)dummy;
    }

    memset(&recv_tmpl, 0, sizeof(recv_tmpl));
    recv_tmpl.msg_control = recv_cmsg;
    recv_tmpl.msg_controllen = sizeof(recv_cmsg);

    {
        struct io_uring_sqe *sqe = wp_uring_get_sqe(r);
        if (!sqe) {
            ret = -ENOSPC;
            goto out_pbuf;
        }
        wp_sqe_recvmsg_multishot(sqe, 1, &recv_tmpl, BGID, 0x52454356ull, true);
        ret = wp_uring_submit(r, 0);
        if (ret < 0)
            goto out_pbuf;
    }

    for (i = 0; i < NMSG; i++) {
        struct io_uring_sqe *sqe;
        unsigned recvd_before = nrecv;

        memset(&smsg[i], 0, sizeof(smsg[i]));
        snprintf(payloads[i], sizeof(payloads[i]), "msg-%u", i);
        iov[i].iov_base = payloads[i];
        iov[i].iov_len = strlen(payloads[i]) + 1;
        smsg[i].msg_iov = &iov[i];
        smsg[i].msg_iovlen = 1;
        if (i == 1) {
            struct cmsghdr *cmsg;
            smsg[i].msg_control = cmsgbuf;
            smsg[i].msg_controllen = sizeof(cmsgbuf);
            cmsg = CMSG_FIRSTHDR(&smsg[i]);
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type = SCM_RIGHTS;
            cmsg->cmsg_len = CMSG_LEN(sizeof(int));
            memcpy(CMSG_DATA(cmsg), &pipefd[0], sizeof(int));
        }
        sqe = wp_uring_get_sqe(r);
        if (!sqe) {
            ret = -ENOSPC;
            goto out_pbuf;
        }
        wp_sqe_sendmsg(sqe, 0, &smsg[i], 0x1000ull + i, true);
        ret = wp_uring_submit(r, 1);
        if (ret < 0)
            goto out_pbuf;
        ret = wait_n(r, 1);
        if (ret < 0)
            goto out_pbuf;

        ready = wp_uring_cq_ready(r);
        for (unsigned k = 0; k < ready; k++) {
            struct io_uring_cqe *cqe = wp_uring_cqe_at(r, k);
            if (cqe->user_data == 0x52454356ull) {
                struct wp_recvmsg_view view;
                int fds[4];
                int nf;

                nrecv++;
                if (cqe->flags & IORING_CQE_F_MORE)
                    nmore++;
                ret = wp_recvmsg_view(&pbuf, cqe, &recv_tmpl, &view);
                if (ret < 0) {
                    printf("      recvmsg view %s res=%d\n", strerror(-ret), cqe->res);
                    g_fail++;
                    continue;
                }
                nf = wp_cmsg_fds(view.ctrl, view.out->controllen, fds, 4);
                if (nf > 0) {
                    nrights++;
                    got_fd = fds[0];
                }
                wp_pbuf_recycle(&pbuf, view.bid);
            } else if (cqe->user_data >= 0x1000ull && cqe->user_data < 0x1000ull + NMSG) {
                nsend++;
                if (cqe->res < 0) {
                    printf("      send res=%d %s\n", cqe->res, strerror(-cqe->res));
                    g_fail++;
                }
            }
        }
        wp_uring_cq_advance(r, ready);
        if (nrecv == recvd_before) {
            ret = wait_n(r, 1);
            if (ret < 0)
                goto out_pbuf;
            i--;
            continue;
        }
    }

    expect(nsend == NMSG, "SENDMSG x3 on registered fd");
    expect(nrecv >= 1, "multishot RECVMSG got stream data");
    printf("      recvs=%u sends=%u (stream may coalesce)\n", nrecv, nsend);
    expect(nmore >= 1, "IORING_CQE_F_MORE on multishot");
    expect(nrights == 1, "SCM_RIGHTS fd received in pbuf cmsg");

    if (got_fd >= 0) {
        char ch = 'K', in = 0;
        ssize_t wr = write(pipefd[1], &ch, 1);
        ssize_t rd = read(got_fd, &in, 1);
        expect(wr == 1 && rd == 1 && in == 'K', "passed fd is a live pipe");
        close(got_fd);
    }

    {
        struct io_uring_sqe *sqe = wp_uring_get_sqe(r);
        if (sqe) {
            sqe->opcode = IORING_OP_ASYNC_CANCEL;
            sqe->addr = 0x52454356ull;
            sqe->user_data = 0x43414e43ull;
            (void)wp_uring_submit(r, 1);
            if (wait_n(r, 1) == 0)
                wp_uring_cq_advance(r, wp_uring_cq_ready(r));
        }
    }
    ret = 0;
out_pbuf:
    wp_pbuf_destroy(r, &pbuf);
close_pipe:
    close(pipefd[0]);
    close(pipefd[1]);
close_socks:
    close(socks[0]);
    close(socks[1]);
    return ret;
}

int main(void)
{
    struct wp_uring ring;
    struct utsname uts;
    int ret;

    uname(&uts);
    printf("wayplot uring step-2  kernel %s %s\n", uts.release, uts.machine);

    ret = wp_uring_setup(&ring, 64);
    expect(ret == 0, "wp_uring_setup");
    if (ret != 0) {
        printf("setup %s\n", strerror(-ret));
        return 1;
    }

    wp_uring_log_caps(&ring, stdout);
    expect(ring.no_sqarray, "NO_SQARRAY");
    expect(ring.ring_reg_index >= 0, "REGISTER_RING_FDS (enter via registered ring)");
    expect((ring.enter_flags & IORING_ENTER_GETEVENTS) != 0, "ENTER_GETEVENTS");
    expect((ring.enter_flags & IORING_ENTER_NO_IOWAIT) != 0, "ENTER_NO_IOWAIT (7.x)");
    expect((ring.params.features & IORING_FEAT_FAST_POLL) != 0, "FAST_POLL");

    ret = test_nop(&ring);
    if (ret < 0) {
        printf("FAIL  NOP (%s)\n", strerror(-ret));
        g_fail++;
    }
    ret = test_timeout(&ring);
    if (ret < 0) {
        printf("FAIL  TIMEOUT (%s)\n", strerror(-ret));
        g_fail++;
    }
    ret = test_multishot_rights(&ring);
    if (ret < 0) {
        printf("FAIL  multishot/rights (%s)\n", strerror(-ret));
        g_fail++;
    }

    expect(*ring.cq_overflow == 0, "CQ overflow counter is 0");
    wp_uring_destroy(&ring);
    expect(ring.fd < 0, "destroy closed ring fd");

    printf("%s  %d failure(s)\n", g_fail ? "RESULT FAIL" : "RESULT PASS", g_fail);
    return g_fail ? 1 : 0;
}

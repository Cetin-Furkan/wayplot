#define _GNU_SOURCE
#include "wayland/conn.h"

#include <errno.h>
#include <linux/io_uring.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int arm_timeout_sqe(struct wp_wl_conn *c, uint64_t timeout_ns)
{
    struct io_uring_sqe *sqe = wp_uring_get_sqe(&c->ring);

    if (!sqe)
        return -ENOBUFS;
    c->wait_ts.tv_sec = (__kernel_time64_t)(timeout_ns / 1000000000ull);
    c->wait_ts.tv_nsec = (__kernel_long_t)(timeout_ns % 1000000000ull);
    sqe->opcode = IORING_OP_TIMEOUT;
    sqe->addr = (uint64_t)(uintptr_t)&c->wait_ts;
    sqe->len = 1;
    sqe->off = 1;
    sqe->user_data = WP_WL_TIME_UD;
    return 0;
}

static int wait_cqe(struct wp_wl_conn *c, unsigned need, uint64_t timeout_ns)
{
    while (wp_uring_cq_ready(&c->ring) < need) {
        int ret;
        int use_ext = timeout_ns && (c->ring.params.features & IORING_FEAT_EXT_ARG);

        if (timeout_ns && !use_ext) {
            ret = arm_timeout_sqe(c, timeout_ns);
            if (ret < 0)
                return ret;
        }
        if (use_ext)
            ret = wp_uring_submit_timeout(&c->ring, need, timeout_ns);
        else
            ret = wp_uring_submit(&c->ring, need);
        if (ret == -ETIME)
            return -ETIME;
        if (ret < 0)
            return ret;
        if (wp_uring_cq_ready(&c->ring) < need)
            return timeout_ns ? -ETIME : -ETIMEDOUT;
    }
    return 0;
}

static struct wp_wl_send_slot *slot_by_ud(struct wp_wl_conn *c, uint64_t ud)
{
    for (unsigned i = 0; i < WP_WL_SEND_SLOTS; i++) {
        if (c->slots[i].busy && c->slots[i].user_data == ud)
            return &c->slots[i];
    }
    return NULL;
}

static struct wp_wl_send_slot *slot_free(struct wp_wl_conn *c)
{
    for (unsigned i = 0; i < WP_WL_SEND_SLOTS; i++) {
        if (!c->slots[i].busy)
            return &c->slots[i];
    }
    return NULL;
}

static void slot_clear(struct wp_wl_send_slot *s)
{
    free(s->heap);
    s->heap = NULL;
    s->busy = false;
    s->user_data = 0;
}

static int in_feed(struct wp_wl_conn *c, const uint8_t *src, size_t n)
{
    if (n == 0)
        return 0;
    if (c->in_len + n > WP_WL_IN_MAX)
        return -EMSGSIZE;
    if (c->in_len + n > c->in_cap) {
        size_t cap = c->in_cap ? c->in_cap : 4096;
        uint8_t *p;
        while (cap < c->in_len + n)
            cap *= 2;
        if (cap > WP_WL_IN_MAX)
            cap = WP_WL_IN_MAX;
        p = realloc(c->in, cap);
        if (!p)
            return -ENOMEM;
        c->in = p;
        c->in_cap = cap;
    }
    memcpy(c->in + c->in_len, src, n);
    c->in_len += n;
    return 0;
}

static void take_cmsg_fds(struct wp_wl_conn *c, const uint8_t *ctrl, size_t controllen)
{
    int tmp[16];
    int n, i;

    n = wp_cmsg_fds(ctrl, controllen, tmp, 16);
    for (i = 0; i < n; i++) {
        if (c->pending_fd_count < 8)
            c->pending_fds[c->pending_fd_count++] = tmp[i];
        else if (tmp[i] >= 0)
            close(tmp[i]);
    }
}

static int reap(struct wp_wl_conn *c)
{
    unsigned ready = wp_uring_cq_ready(&c->ring);
    unsigned i;
    int err = 0;

    for (i = 0; i < ready; i++) {
        struct io_uring_cqe *cqe = wp_uring_cqe_at(&c->ring, i);

        if (cqe->user_data == WP_WL_RECV_UD) {
            struct wp_recvmsg_view v;
            int vr;

            if (cqe->res == -ENOBUFS) {
                c->recv_armed = false;
                continue;
            }
            if (cqe->res <= 0) {
                c->recv_armed = false;
                if (!err)
                    err = cqe->res == 0 ? -EPIPE : cqe->res;
                continue;
            }
            vr = wp_recvmsg_view(&c->pbuf, cqe, &c->recv_tmpl, &v);
            if (vr < 0) {
                if (cqe->flags & IORING_CQE_F_BUFFER) {
                    uint16_t bid = (uint16_t)(cqe->flags >> IORING_CQE_BUFFER_SHIFT);
                    wp_pbuf_recycle(&c->pbuf, bid);
                }
                if (!err)
                    err = vr;
                if (!(cqe->flags & IORING_CQE_F_MORE))
                    c->recv_armed = false;
                continue;
            }
            if (v.out->controllen >= sizeof(struct cmsghdr))
                take_cmsg_fds(c, v.ctrl, v.out->controllen);
            vr = in_feed(c, v.payload, v.out->payloadlen);
            wp_pbuf_recycle(&c->pbuf, v.bid);
            if (vr < 0 && !err)
                err = vr;
            if (!(cqe->flags & IORING_CQE_F_MORE))
                c->recv_armed = false;
        } else if ((cqe->user_data & ~0xffull) == WP_WL_POLL_UD) {
            unsigned slot = (unsigned)(cqe->user_data & 0xffu);
            if (cqe->res >= 0)
                c->poll_ready |= 1u << slot;
        } else if (cqe->user_data == WP_WL_TIME_UD) {
            /* timeout CQE; wait_cqe already observed -ETIME via EXT_ARG */
        } else {
            struct wp_wl_send_slot *s = slot_by_ud(c, cqe->user_data);
            if (s) {
                if (cqe->res < 0) {
                    if (!err)
                        err = cqe->res;
                } else if ((size_t)cqe->res != s->len) {
                    if (!err)
                        err = -EIO;
                }
                slot_clear(s);
            }
        }
    }
    wp_uring_cq_advance(&c->ring, ready);
    if (err)
        return err;
    return (int)ready;
}

static int arm_recv(struct wp_wl_conn *c)
{
    struct io_uring_sqe *sqe;

    if (c->recv_armed)
        return 0;
    sqe = wp_uring_get_sqe(&c->ring);
    if (!sqe)
        return -ENOBUFS;
    wp_sqe_recvmsg_multishot(sqe, c->fd_slot, &c->recv_tmpl,
                             WP_WL_PBUF_BGID, WP_WL_RECV_UD, true);
    c->recv_armed = true;
    return 0;
}

int wp_wl_conn_init(struct wp_wl_conn *c)
{
    int ret;

    if (!c)
        return -EINVAL;
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    c->fd_slot = 0;
    c->next_id = 1;
    ret = wp_map_set(&c->map, 1, WP_OBJ_DISPLAY, 1);
    if (ret < 0)
        return ret;
    ret = wp_uring_setup(&c->ring, 64);
    if (ret < 0) {
        wp_map_free(&c->map);
        return ret;
    }
    ret = wp_pbuf_setup(&c->ring, &c->pbuf, WP_WL_PBUF_COUNT, WP_WL_PBUF_SIZE, WP_WL_PBUF_BGID);
    if (ret < 0) {
        wp_uring_destroy(&c->ring);
        wp_map_free(&c->map);
        return ret;
    }
    c->recv_tmpl.msg_control = c->recv_cmsg;
    c->recv_tmpl.msg_controllen = sizeof(c->recv_cmsg);
    return 0;
}

int wp_wl_conn_adopt(struct wp_wl_conn *c, int fd)
{
    int ret;

    if (!c || fd < 0)
        return -EINVAL;
    c->fd = fd;
    ret = wp_uring_register_files(&c->ring, &c->fd, 1);
    if (ret < 0)
        return ret;
    ret = arm_recv(c);
    if (ret < 0)
        return ret;
    ret = wp_uring_submit(&c->ring, 0);
    if (ret < 0)
        return ret;
    return 0;
}

static int wait_ud(struct wp_wl_conn *c, uint64_t ud, int *res_out)
{
    const uint64_t timeout_ns = 2000000000ull;
    int spins = 0;

    while (spins++ < 8) {
        unsigned ready, i;
        int ret = wait_cqe(c, 1, timeout_ns);
        if (ret < 0)
            return ret;
        ready = wp_uring_cq_ready(&c->ring);
        for (i = 0; i < ready; i++) {
            struct io_uring_cqe *cqe = wp_uring_cqe_at(&c->ring, i);
            if (cqe->user_data == ud) {
                *res_out = cqe->res;
                wp_uring_cq_advance(&c->ring, ready);
                return 0;
            }
        }
        wp_uring_cq_advance(&c->ring, ready);
    }
    return -ETIMEDOUT;
}

int wp_wl_display_path(char *out, size_t cap)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    const char *disp = getenv("WAYLAND_DISPLAY");

    if (!out || cap < 8)
        return -EINVAL;
    if (!runtime || !disp || disp[0] == '\0')
        return -ENOENT;
    if (disp[0] == '/') {
        if (strlen(disp) >= cap)
            return -ENAMETOOLONG;
        memcpy(out, disp, strlen(disp) + 1);
        return 0;
    }
    if (snprintf(out, cap, "%s/%s", runtime, disp) >= (int)cap)
        return -ENAMETOOLONG;
    return 0;
}

static int connect_libc(struct wp_wl_conn *c, const struct sockaddr_un *addr)
{
    int sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock < 0)
        return -errno;
    if (connect(sock, (const struct sockaddr *)addr, sizeof(*addr)) < 0) {
        int e = -errno;
        close(sock);
        return e;
    }
    return wp_wl_conn_adopt(c, sock);
}

int wp_wl_conn_connect(struct wp_wl_conn *c, const char *path)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    struct io_uring_sqe *sqe;
    int ret, sock, cres;

    if (!c || !path || path[0] == '\0')
        return -EINVAL;
    if (!c->pbuf.ring) {
        ret = wp_wl_conn_init(c);
        if (ret < 0)
            return ret;
    }
    if (strlen(path) >= sizeof(addr.sun_path))
        return -ENAMETOOLONG;
    memcpy(addr.sun_path, path, strlen(path) + 1);

    sqe = wp_uring_get_sqe(&c->ring);
    if (!sqe)
        return connect_libc(c, &addr);
    sqe->opcode = IORING_OP_SOCKET;
    sqe->fd = AF_UNIX;
    sqe->off = (uint64_t)(unsigned)(SOCK_STREAM | SOCK_CLOEXEC);
    sqe->len = 0;
    sqe->user_data = 0x534F434Bull;
    if (wp_uring_submit(&c->ring, 1) < 0)
        return connect_libc(c, &addr);
    if (wait_ud(c, 0x534F434Bull, &sock) < 0 || sock < 0)
        return connect_libc(c, &addr);

    sqe = wp_uring_get_sqe(&c->ring);
    if (!sqe) {
        close(sock);
        return connect_libc(c, &addr);
    }
    sqe->opcode = IORING_OP_CONNECT;
    sqe->fd = sock;
    sqe->addr = (uint64_t)(uintptr_t)&addr;
    sqe->off = sizeof(addr);
    sqe->user_data = 0x434F4E4Eull;
    if (wp_uring_submit(&c->ring, 1) < 0) {
        close(sock);
        return connect_libc(c, &addr);
    }
    if (wait_ud(c, 0x434F4E4Eull, &cres) < 0 || cres < 0) {
        close(sock);
        return connect_libc(c, &addr);
    }
    return wp_wl_conn_adopt(c, sock);
}

int wp_wl_conn_pair(struct wp_wl_conn *a, struct wp_wl_conn *b)
{
    int fds[2];
    int ret;

    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) < 0)
        return -errno;
    ret = wp_wl_conn_init(a);
    if (ret < 0) {
        close(fds[0]);
        close(fds[1]);
        return ret;
    }
    ret = wp_wl_conn_init(b);
    if (ret < 0) {
        wp_wl_conn_destroy(a);
        close(fds[0]);
        close(fds[1]);
        return ret;
    }
    ret = wp_wl_conn_adopt(a, fds[0]);
    if (ret < 0) {
        wp_wl_conn_destroy(a);
        wp_wl_conn_destroy(b);
        close(fds[1]);
        return ret;
    }
    ret = wp_wl_conn_adopt(b, fds[1]);
    if (ret < 0) {
        wp_wl_conn_destroy(a);
        wp_wl_conn_destroy(b);
        return ret;
    }
    return 0;
}

void wp_wl_conn_destroy(struct wp_wl_conn *c)
{
    unsigned i;

    if (!c)
        return;
    for (i = 0; i < WP_WL_SEND_SLOTS; i++)
        slot_clear(&c->slots[i]);
    for (i = 0; i < (unsigned)c->pending_fd_count; i++) {
        if (c->pending_fds[i] >= 0)
            close(c->pending_fds[i]);
    }
    wp_pbuf_destroy(&c->ring, &c->pbuf);
    wp_uring_destroy(&c->ring);
    wp_map_free(&c->map);
    if (c->fd >= 0)
        close(c->fd);
    free(c->in);
    memset(c, 0, sizeof(*c));
    c->fd = -1;
}

int wp_wl_send(struct wp_wl_conn *c, const void *data, size_t len,
               const int *fds, int n_fds)
{
    struct wp_wl_send_slot *s;
    struct io_uring_sqe *sqe;
    uint8_t *buf;
    int ret;

    if (!c || !data || len == 0 || n_fds < 0 || n_fds > 8)
        return -EINVAL;
    if (n_fds > 0 && !fds)
        return -EINVAL;

    s = slot_free(c);
    if (!s) {
        ret = wp_wl_pump(c, 1);
        if (ret < 0)
            return ret;
        s = slot_free(c);
        if (!s)
            return -EBUSY;
    }

    if (len <= WP_WL_SEND_INLINE) {
        memcpy(s->inline_buf, data, len);
        buf = s->inline_buf;
        s->heap = NULL;
    } else {
        buf = malloc(len);
        if (!buf)
            return -ENOMEM;
        memcpy(buf, data, len);
        s->heap = buf;
    }

    memset(&s->msg, 0, sizeof(s->msg));
    s->iov.iov_base = buf;
    s->iov.iov_len = len;
    s->msg.msg_iov = &s->iov;
    s->msg.msg_iovlen = 1;
    if (n_fds > 0) {
        struct cmsghdr *ch;
        s->msg.msg_control = s->cmsg;
        s->msg.msg_controllen = (socklen_t)CMSG_SPACE(sizeof(int) * (size_t)n_fds);
        ch = CMSG_FIRSTHDR(&s->msg);
        ch->cmsg_level = SOL_SOCKET;
        ch->cmsg_type = SCM_RIGHTS;
        ch->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)n_fds);
        memcpy(CMSG_DATA(ch), fds, sizeof(int) * (size_t)n_fds);
    }

    sqe = wp_uring_get_sqe(&c->ring);
    if (!sqe) {
        ret = wp_wl_pump(c, 1);
        if (ret < 0) {
            slot_clear(s);
            return ret;
        }
        sqe = wp_uring_get_sqe(&c->ring);
        if (!sqe) {
            slot_clear(s);
            return -ENOBUFS;
        }
    }

    s->len = len;
    s->user_data = WP_WL_SEND_UD | (uint64_t)++c->send_seq;
    s->busy = true;
    wp_sqe_sendmsg(sqe, c->fd_slot, &s->msg, s->user_data, true);
    ret = wp_uring_submit(&c->ring, 0);
    if (ret < 0)
        return ret;
    return 0;
}

int wp_wl_pump_wait(struct wp_wl_conn *c, unsigned min_cqe, uint64_t timeout_ns)
{
    int ret;

    if (!c)
        return -EINVAL;
    if (!c->recv_armed) {
        ret = arm_recv(c);
        if (ret < 0)
            return ret;
    }
    if (min_cqe > 0) {
        ret = wait_cqe(c, min_cqe, timeout_ns);
        if (ret < 0)
            return ret;
    } else {
        ret = wp_uring_submit(&c->ring, 0);
        if (ret < 0)
            return ret;
    }
    return reap(c);
}

int wp_wl_pump(struct wp_wl_conn *c, unsigned min_cqe)
{
    return wp_wl_pump_wait(c, min_cqe, 0);
}

bool wp_wl_peek(struct wp_wl_conn *c, struct wp_wl_msg *m)
{
    const uint32_t *hdr;
    uint16_t size;

    if (!c || !m || c->in_len < 8)
        return false;
    hdr = (const uint32_t *)c->in;
    size = (uint16_t)(hdr[1] >> 16);
    if (size < 8 || (size & 3u) != 0 || (size_t)size > c->in_len)
        return false;
    m->obj = hdr[0];
    m->opcode = (uint16_t)(hdr[1] & 0xffffu);
    m->size = size;
    m->raw = c->in;
    m->body = c->in + 8;
    m->body_len = (uint16_t)(size - 8);
    return true;
}

uint32_t wp_wl_alloc_id(struct wp_wl_conn *c)
{
    return ++c->next_id;
}

int wp_wl_take_fd(struct wp_wl_conn *c)
{
    int fd;
    if (!c || c->pending_fd_count <= 0)
        return -1;
    fd = c->pending_fds[0];
    c->pending_fd_count--;
    if (c->pending_fd_count)
        memmove(c->pending_fds, c->pending_fds + 1,
                (size_t)c->pending_fd_count * sizeof(int));
    return fd;
}

void wp_wl_consume(struct wp_wl_conn *c)
{
    struct wp_wl_msg m;

    if (!wp_wl_peek(c, &m))
        return;
    c->in_len -= m.size;
    if (c->in_len)
        memmove(c->in, c->in + m.size, c->in_len);
}

int wp_wl_poll_add(struct wp_wl_conn *c, int fd, unsigned events, unsigned slot)
{
    struct io_uring_sqe *sqe;
    int ret;

    if (!c || fd < 0 || slot > 31)
        return -EINVAL;
    sqe = wp_uring_get_sqe(&c->ring);
    if (!sqe) {
        ret = wp_wl_pump(c, 1);
        if (ret < 0)
            return ret;
        sqe = wp_uring_get_sqe(&c->ring);
        if (!sqe)
            return -ENOBUFS;
    }
    wp_sqe_poll_add(sqe, fd, events, WP_WL_POLL_UD | (uint64_t)slot);
    ret = wp_uring_submit(&c->ring, 0);
    if (ret < 0)
        return ret;
    return 0;
}

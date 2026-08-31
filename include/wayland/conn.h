#ifndef WAYLAND_CONN_H
#define WAYLAND_CONN_H

#include "uring/pbuf.h"
#include "uring/ring.h"
#include "uring/sock.h"
#include "wayland/map.h"

#include <linux/time_types.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

/* Small Wayland requests are 8–32 bytes. Max wire size is 16-bit.
 * Recv uses 8 KiB provided buffers: a 64 KiB message is several CQEs,
 * assembled here. That beats 256×64 KiB of idle RAM. */
#define WP_WL_PBUF_COUNT 64
#define WP_WL_PBUF_SIZE  8192
#define WP_WL_PBUF_BGID  1
#define WP_WL_SEND_SLOTS 32
#define WP_WL_SEND_INLINE 256
#define WP_WL_CMSG_BYTES (CMSG_SPACE(sizeof(int) * 8))
#define WP_WL_IN_MAX     (65536u * 4u)
#define WP_WL_RECV_UD    0x574C5256ull
#define WP_WL_SEND_UD    0x53454E00ull
#define WP_WL_TIME_UD    0x54494D45ull
#define WP_WL_POLL_UD    0x504F4C00ull

struct wp_wl_msg {
    uint32_t obj;
    uint16_t opcode;
    uint16_t size;
    const uint8_t *raw;
    const uint8_t *body;
    uint16_t body_len;
};

struct wp_wl_send_slot {
    struct msghdr msg;
    struct iovec iov;
    uint8_t cmsg[WP_WL_CMSG_BYTES];
    uint8_t inline_buf[WP_WL_SEND_INLINE];
    uint8_t *heap;
    size_t len;
    uint64_t user_data;
    bool busy;
};

struct wp_wl_conn {
    struct wp_uring ring;
    struct wp_pbuf pbuf;
    struct wp_map map;
    int fd;
    int fd_slot;
    bool recv_armed;
    struct msghdr recv_tmpl;
    uint8_t recv_cmsg[WP_WL_CMSG_BYTES];
    struct wp_wl_send_slot slots[WP_WL_SEND_SLOTS];
    uint32_t send_seq;
    uint32_t next_id;
    uint8_t *in;
    size_t in_len;
    size_t in_cap;
    int pending_fds[8];
    int pending_fd_count;
    struct __kernel_timespec wait_ts;
    uint32_t poll_ready;
};

[[nodiscard]] int wp_wl_conn_init(struct wp_wl_conn *c);
[[nodiscard]] int wp_wl_conn_adopt(struct wp_wl_conn *c, int fd);
[[nodiscard]] int wp_wl_conn_pair(struct wp_wl_conn *a, struct wp_wl_conn *b);
[[nodiscard]] int wp_wl_display_path(char *out, size_t cap);
[[nodiscard]] int wp_wl_conn_connect(struct wp_wl_conn *c, const char *path);
void wp_wl_conn_destroy(struct wp_wl_conn *c);

[[nodiscard]] int wp_wl_send(struct wp_wl_conn *c, const void *data, size_t len,
                             const int *fds, int n_fds);
[[nodiscard]] int wp_wl_pump(struct wp_wl_conn *c, unsigned min_cqe);
[[nodiscard]] int wp_wl_pump_wait(struct wp_wl_conn *c, unsigned min_cqe, uint64_t timeout_ns);
[[nodiscard]] bool wp_wl_peek(struct wp_wl_conn *c, struct wp_wl_msg *m);
void wp_wl_consume(struct wp_wl_conn *c);
[[nodiscard]] uint32_t wp_wl_alloc_id(struct wp_wl_conn *c);
[[nodiscard]] int wp_wl_take_fd(struct wp_wl_conn *c);
[[nodiscard]] int wp_wl_poll_add(struct wp_wl_conn *c, int fd, unsigned events, unsigned slot);

#endif /* WAYLAND_CONN_H */

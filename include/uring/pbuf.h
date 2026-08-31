#ifndef URING_PBUF_H
#define URING_PBUF_H

#include "uring/ring.h"

#include <stdint.h>

struct wp_pbuf {
    struct io_uring_buf_ring *ring;
    uint8_t *pool;
    size_t ring_sz;
    uint32_t buf_size;
    uint16_t count;
    uint16_t bgid;
    uint16_t tail;
};

[[nodiscard]] int wp_pbuf_setup(struct wp_uring *r, struct wp_pbuf *p,
                                uint16_t count, uint32_t buf_size, uint16_t bgid);
void wp_pbuf_destroy(struct wp_uring *r, struct wp_pbuf *p);
void wp_pbuf_recycle(struct wp_pbuf *p, uint16_t bid);
[[nodiscard]] uint8_t *wp_pbuf_ptr(const struct wp_pbuf *p, uint16_t bid);

#endif /* URING_PBUF_H */

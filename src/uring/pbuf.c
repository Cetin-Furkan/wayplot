#define _GNU_SOURCE
#include "uring/pbuf.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static void enqueue(struct wp_pbuf *p, uint16_t bid)
{
    uint16_t idx = p->tail & (uint16_t)(p->count - 1u);
    struct io_uring_buf *slot = &p->ring->bufs[idx];

    slot->addr = (uint64_t)(uintptr_t)(p->pool + (size_t)bid * p->buf_size);
    slot->len = p->buf_size;
    slot->bid = bid;
    p->tail++;
}

static void publish(struct wp_pbuf *p)
{
    atomic_store_explicit((_Atomic uint16_t *)&p->ring->tail, p->tail, memory_order_release);
}

int wp_pbuf_setup(struct wp_uring *r, struct wp_pbuf *p,
                  uint16_t count, uint32_t buf_size, uint16_t bgid)
{
    struct io_uring_buf_reg reg;
    void *ring_mem;
    int ret;

    if (!r || !p || count == 0 || (count & (count - 1u)) != 0 || buf_size == 0)
        return -EINVAL;

    memset(p, 0, sizeof(*p));
    p->count = count;
    p->buf_size = buf_size;
    p->bgid = bgid;
    p->ring_sz = sizeof(struct io_uring_buf) * (size_t)count;

    ring_mem = mmap(NULL, p->ring_sz, PROT_READ | PROT_WRITE,
                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (ring_mem == MAP_FAILED)
        return -errno;

    memset(&reg, 0, sizeof(reg));
    reg.ring_addr = (uint64_t)(uintptr_t)ring_mem;
    reg.ring_entries = count;
    reg.bgid = bgid;
    ret = wp_uring_register(r, IORING_REGISTER_PBUF_RING, &reg, 1);
    if (ret < 0) {
        munmap(ring_mem, p->ring_sz);
        return ret;
    }

    p->pool = aligned_alloc(4096, (size_t)count * buf_size);
    if (!p->pool) {
        (void)wp_uring_register(r, IORING_UNREGISTER_PBUF_RING, &reg, 1);
        munmap(ring_mem, p->ring_sz);
        return -ENOMEM;
    }

    p->ring = (struct io_uring_buf_ring *)ring_mem;
    p->tail = 0;
    for (uint16_t i = 0; i < count; i++)
        enqueue(p, i);
    publish(p);
    return 0;
}

void wp_pbuf_destroy(struct wp_uring *r, struct wp_pbuf *p)
{
    struct io_uring_buf_reg reg;

    if (!p)
        return;
    if (r && r->fd >= 0 && p->ring) {
        memset(&reg, 0, sizeof(reg));
        reg.bgid = p->bgid;
        (void)wp_uring_register(r, IORING_UNREGISTER_PBUF_RING, &reg, 1);
    }
    if (p->ring)
        munmap(p->ring, p->ring_sz);
    free(p->pool);
    memset(p, 0, sizeof(*p));
}

void wp_pbuf_recycle(struct wp_pbuf *p, uint16_t bid)
{
    if (!p || bid >= p->count)
        return;
    enqueue(p, bid);
    publish(p);
}

uint8_t *wp_pbuf_ptr(const struct wp_pbuf *p, uint16_t bid)
{
    if (!p || bid >= p->count)
        return NULL;
    return p->pool + (size_t)bid * p->buf_size;
}

#ifndef KHOROS_URING_PBUF_H
#define KHOROS_URING_PBUF_H

#if __STDC_VERSION__ < 202311L
#error "Khoros requires pure ISO C23 (compile with -std=c23)"
#endif

#ifdef _STDBOOL_H
#error "<stdbool.h> is strictly banned. In C23, bool, true, and false are native language keywords."
#endif

#ifdef _STDALIGN_H
#error "<stdalign.h> is strictly banned. In C23, alignas and alignof are native language keywords."
#endif

#include <stdint.h>
#include <stddef.h>
#include "khoros/core/attributes.h"
#include "khoros/uring/ring.h"

constexpr uint16_t KHR_PBUF_TIER0_BGID  = 0;
constexpr uint16_t KHR_PBUF_TIER1_BGID  = 1;
constexpr uint32_t KHR_PBUF_TIER0_COUNT = 128;
constexpr uint32_t KHR_PBUF_TIER0_SIZE  = 256;
constexpr uint32_t KHR_PBUF_TIER1_COUNT = 16;
constexpr uint32_t KHR_PBUF_TIER1_SIZE  = 65'536; /* 64 KiB */
constexpr uint16_t KHR_PBUF_NO_SELECT   = 0xFFFF;

typedef struct {
    uint16_t bgid;
    uint32_t entries;
    uint32_t buf_size;
    uint32_t tail;
    struct io_uring_buf_ring* br;
    size_t   br_map_sz;
    uint8_t* data;
    size_t   data_sz;
    khr_uring_t* ring;
    bool     registered;
} khr_pbuf_t;

typedef struct {
    const struct io_uring_recvmsg_out* hdr;
    const uint8_t* name;
    const uint8_t* control;
    const uint8_t* payload;
    uint32_t payload_len;
} khr_recvmsg_view_t;

[[nodiscard]]
bool khr_pbuf_init(khr_pbuf_t* pbuf, khr_uring_t* ring, uint16_t bgid,
                   uint32_t entries, uint32_t buf_size);

void khr_pbuf_destroy(khr_pbuf_t* pbuf);

void khr_pbuf_recycle(khr_pbuf_t* pbuf, uint16_t bid);

[[nodiscard]]
uint8_t* khr_pbuf_data(khr_pbuf_t* pbuf, uint16_t bid);

[[nodiscard]]
bool khr_recvmsg_parse(const uint8_t* buf, size_t cap,
                       uint32_t msg_namelen, uint32_t msg_controllen,
                       khr_recvmsg_view_t* out);

#endif /* KHOROS_URING_PBUF_H */

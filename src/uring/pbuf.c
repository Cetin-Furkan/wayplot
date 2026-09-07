#include "khoros/uring/pbuf.h"
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#include <errno.h>

[[nodiscard]]
static bool khr_is_pow2(uint32_t n) {
    return n >= 2 && (n & (n - 1U)) == 0;
}

[[nodiscard]]
bool khr_pbuf_init(khr_pbuf_t* pbuf, khr_uring_t* ring, uint16_t bgid,
                   uint32_t entries, uint32_t buf_size) {
    if (pbuf == nullptr || ring == nullptr || ring->ring_fd < 0 ||
        !khr_is_pow2(entries) || buf_size == 0) {
        return false;
    }
    *pbuf = (khr_pbuf_t){
        .bgid = bgid,
        .entries = entries,
        .buf_size = buf_size,
        .ring = ring,
    };

    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) {
        page = 4'096;
    }
    size_t page_sz = (size_t)page;

    size_t br_bytes = (size_t)entries * sizeof(struct io_uring_buf);
    size_t br_map = (br_bytes + page_sz - 1U) & ~(page_sz - 1U);
    void* br_ptr = mmap(nullptr, br_map, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (br_ptr == MAP_FAILED) {
        return false;
    }
    memset(br_ptr, 0, br_map);

    size_t data_bytes = (size_t)entries * (size_t)buf_size;
    size_t data_map = (data_bytes + page_sz - 1U) & ~(page_sz - 1U);
    void* data_ptr = mmap(nullptr, data_map, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (data_ptr == MAP_FAILED) {
        munmap(br_ptr, br_map);
        return false;
    }
    memset(data_ptr, 0, data_map);

    pbuf->br = (struct io_uring_buf_ring*)br_ptr;
    pbuf->br_map_sz = br_map;
    pbuf->data = (uint8_t*)data_ptr;
    pbuf->data_sz = data_map;

    struct io_uring_buf_reg reg = {
        .ring_addr = (uint64_t)br_ptr,
        .ring_entries = entries,
        .bgid = bgid,
    };
    int reg_rc = -1;
    constexpr int MAX_RETRIES = 100;
    for (int retry = 0; retry < MAX_RETRIES; retry++) {
        reg_rc = khr_sys_io_uring_register(ring->ring_fd, IORING_REGISTER_PBUF_RING, &reg, 1);
        if (reg_rc == 0) {
            break;
        }
        if (errno == ENOMEM || errno == EAGAIN || errno == EBUSY) {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 1'000'000 };
            nanosleep(&ts, nullptr);
        } else {
            break;
        }
    }
    if (reg_rc != 0) {
        munmap(data_ptr, data_map);
        munmap(br_ptr, br_map);
        pbuf->br = nullptr;
        pbuf->data = nullptr;
        return false;
    }
    pbuf->registered = true;

    uint32_t mask = entries - 1U;
    for (uint32_t i = 0; i < entries; i++) {
        struct io_uring_buf* b = &pbuf->br->bufs[(pbuf->tail + i) & mask];
        b->addr = (uint64_t)(pbuf->data + (size_t)i * (size_t)buf_size);
        b->len = buf_size;
        b->bid = (uint16_t)i;
    }
    pbuf->tail += entries;
    atomic_store_explicit((_Atomic uint16_t*)&pbuf->br->tail,
                          (uint16_t)pbuf->tail, memory_order_release);
    return true;
}

void khr_pbuf_destroy(khr_pbuf_t* pbuf) {
    if (pbuf == nullptr) {
        return;
    }
    if (pbuf->registered && pbuf->ring != nullptr && pbuf->ring->ring_fd >= 0) {
        struct io_uring_buf_reg reg = { .bgid = pbuf->bgid };
        (void)khr_sys_io_uring_register(pbuf->ring->ring_fd,
                                        IORING_UNREGISTER_PBUF_RING, &reg, 1);
        pbuf->registered = false;
    }
    if (pbuf->data != nullptr) {
        munmap(pbuf->data, pbuf->data_sz);
        pbuf->data = nullptr;
    }
    if (pbuf->br != nullptr) {
        munmap(pbuf->br, pbuf->br_map_sz);
        pbuf->br = nullptr;
    }
}

void khr_pbuf_recycle(khr_pbuf_t* pbuf, uint16_t bid) {
    if (pbuf == nullptr || pbuf->br == nullptr || bid >= pbuf->entries) {
        return;
    }
    uint32_t mask = pbuf->entries - 1U;
    struct io_uring_buf* b = &pbuf->br->bufs[pbuf->tail & mask];
    b->addr = (uint64_t)(pbuf->data + (size_t)bid * (size_t)pbuf->buf_size);
    b->len = pbuf->buf_size;
    b->bid = bid;
    pbuf->tail++;
    atomic_store_explicit((_Atomic uint16_t*)&pbuf->br->tail,
                          (uint16_t)pbuf->tail, memory_order_release);
}

[[nodiscard]]
uint8_t* khr_pbuf_data(khr_pbuf_t* pbuf, uint16_t bid) {
    if (pbuf == nullptr || pbuf->data == nullptr || bid >= pbuf->entries) {
        return nullptr;
    }
    return pbuf->data + (size_t)bid * (size_t)pbuf->buf_size;
}

[[nodiscard]]
bool khr_recvmsg_parse(const uint8_t* buf, size_t cap,
                       uint32_t msg_namelen, uint32_t msg_controllen,
                       khr_recvmsg_view_t* out) {
    if (buf == nullptr || out == nullptr || cap < sizeof(struct io_uring_recvmsg_out)) {
        return false;
    }
    auto hdr = (const struct io_uring_recvmsg_out*)buf;
    size_t off = sizeof(struct io_uring_recvmsg_out);
    if (off + (size_t)msg_namelen + (size_t)msg_controllen > cap) {
        return false;
    }
    const uint8_t* name = buf + off;
    off += msg_namelen;
    const uint8_t* control = buf + off;
    off += msg_controllen;
    const uint8_t* payload = buf + off;
    if (off + hdr->payloadlen > cap) {
        return false;
    }
    *out = (khr_recvmsg_view_t){
        .hdr = hdr,
        .name = name,
        .control = control,
        .payload = payload,
        .payload_len = hdr->payloadlen,
    };
    return true;
}

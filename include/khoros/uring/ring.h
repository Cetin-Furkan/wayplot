#ifndef KHOROS_URING_RING_H
#define KHOROS_URING_RING_H

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
#include <stdatomic.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <linux/io_uring.h>
#include <linux/time_types.h>
#include "khoros/core/attributes.h"
#include "khoros/uring/raw_syscalls.h"

/* Ring A — real-time presentation (Wayplot topology) */
constexpr uint32_t KHR_RING_A_SQ_ENTRIES = 64;
constexpr uint32_t KHR_RING_A_CQ_ENTRIES = 256;

/* Ring B — throughput / asset streaming */
constexpr uint32_t KHR_RING_B_SQ_ENTRIES = 128;
constexpr uint32_t KHR_RING_B_CQ_ENTRIES = 256;

constexpr uint32_t KHR_POLLIN = 0x0001U;

constexpr uint64_t KHR_TAG_MSG_RING     = 0x4D53'4752ULL; /* 'MSGR' */
constexpr uint64_t KHR_TAG_NOP          = 0x4E4F'5030ULL; /* 'NOP0' */
constexpr uint64_t KHR_TAG_TIMEOUT      = 0x5449'4D45ULL; /* 'TIME' */
constexpr uint64_t KHR_TAG_EVENTFD      = 0x4556'4644ULL; /* 'EVFD' */
constexpr uint64_t KHR_TAG_RECVMSG      = 0x5256'434DULL; /* 'RVCM' (tier 0) */
constexpr uint64_t KHR_TAG_RECVMSG_T1   = 0x5256'4331ULL; /* 'RVC1' (tier 1) */
constexpr uint64_t KHR_TAG_SENDMSG      = 0x534E'444DULL; /* 'SNDM' */
constexpr uint64_t KHR_TAG_INGEST_STATX = 0x5354'5831ULL; /* 'STX1' */
constexpr uint64_t KHR_TAG_INGEST_OPEN  = 0x4F50'4E32ULL; /* 'OPN2' */
constexpr uint64_t KHR_TAG_INGEST_READ  = 0x5244'4658ULL; /* 'RDFX' */
constexpr uint64_t KHR_TAG_INGEST_CLOSE = 0x434C'4F53ULL; /* 'CLOS' */
constexpr uint64_t KHR_TAG_FD_INSTALL   = 0x4644'494EULL; /* 'FDIN' */
constexpr uint64_t KHR_TAG_WL_SEND      = 0x574C'534EULL; /* 'WLSN' */
constexpr uint64_t KHR_TAG_WL_RECV      = 0x574C'5243ULL; /* 'WLRC' (tier 0) */
constexpr uint64_t KHR_TAG_WL_RECV_T1   = 0x574C'5231ULL; /* 'WLR1' (tier 1) */
constexpr uint64_t KHR_TAG_WL_SOCKET    = 0x574C'534BULL; /* 'WLSK' */
constexpr uint64_t KHR_TAG_WL_CONNECT   = 0x574C'434EULL; /* 'WLCN' */
constexpr uint64_t KHR_TAG_CLOSE        = 0x434C'5330ULL; /* 'CLS0' */
constexpr uint64_t KHR_TAG_ACCEPT       = 0x4143'5054ULL; /* 'ACPT' */

constexpr uint32_t KHR_DIRECT_SLOT_INGEST   = 0;
constexpr uint32_t KHR_DIRECT_SLOT_WAYLAND  = 1;
constexpr uint32_t KHR_DEFAULT_SPARSE_FILES = 16;

constexpr uint32_t KHR_MSG_RES_BDA    = 0x0000'BDA0U;
constexpr uint32_t KHR_MSG_RES_INGEST = 0x0000'1E57U;
/* Core 0 -> Core 1 worker wakeup. Posted with IORING_OP_MSG_RING from Ring A
 * into Ring B; carries no payload (user_data 0). The worker treats it as a
 * pure doorbell and drains the SPSC cmdq. */
constexpr uint32_t KHR_MSG_RES_WAKE   = 0x0000'574BU; /* 'WK' */
constexpr uint64_t KHR_TAG_WAKE       = 0x5741'4B45ULL; /* 'WAKE' */

typedef struct {
    uint32_t sq_entries;
    uint32_t cq_entries;
    uint32_t flags;
    bool     register_ring_fd;
    uint32_t registered_files_nr;
} khr_uring_config_t;

typedef struct {
    int      ring_fd;
    uint32_t enter_fd; /* 0 if registered, ring_fd otherwise */
    uint32_t enter_flags;
    uint32_t features;

    size_t   ring_sz;
    size_t   sqes_sz;
    uint8_t* ring_ptr;
    struct io_uring_sqe* sqes;

    /* Submission Queue Mappings */
    _Atomic uint32_t* sq_khead;
    _Atomic uint32_t* sq_ktail;
    uint32_t* sq_kflags;
    uint32_t* sq_karray;
    uint32_t  sq_mask;
    uint32_t  sq_entries;
    uint32_t  sq_tail; /* Local cached tail */

    /* Completion Queue Mappings */
    _Atomic uint32_t* cq_khead;
    _Atomic uint32_t* cq_ktail;
    uint32_t* cq_kflags;
    uint32_t* cq_koverflow;
    struct io_uring_cqe* cq_cqes;
    uint32_t  cq_mask;
    uint32_t  cq_entries;
    uint32_t  cq_head; /* Local cached head */

    uint32_t  setup_flags;
    bool      no_sqarray;
    bool      clock_monotonic;
    bool      files_registered;
    uint32_t  registered_files_count;
} khr_uring_t;

[[nodiscard]]
static inline khr_uring_config_t khr_uring_config_ring_a(void) {
    return (khr_uring_config_t){
        .sq_entries = KHR_RING_A_SQ_ENTRIES,
        .cq_entries = KHR_RING_A_CQ_ENTRIES,
        .flags = IORING_SETUP_SINGLE_ISSUER |
                 IORING_SETUP_DEFER_TASKRUN |
                 IORING_SETUP_COOP_TASKRUN  |
                 IORING_SETUP_NO_SQARRAY    |
                 IORING_SETUP_CLAMP         |
                 IORING_SETUP_CQSIZE,
        .register_ring_fd = true,
        .registered_files_nr = KHR_DEFAULT_SPARSE_FILES,
    };
}

[[nodiscard]]
static inline khr_uring_config_t khr_uring_config_ring_b(void) {
    return (khr_uring_config_t){
        .sq_entries = KHR_RING_B_SQ_ENTRIES,
        .cq_entries = KHR_RING_B_CQ_ENTRIES,
        .flags = IORING_SETUP_SINGLE_ISSUER |
                 IORING_SETUP_DEFER_TASKRUN |
                 IORING_SETUP_NO_SQARRAY    |
                 IORING_SETUP_CQSIZE,
        .register_ring_fd = true,
        .registered_files_nr = KHR_DEFAULT_SPARSE_FILES,
    };
}

[[nodiscard]]
static inline uint16_t khr_cqe_buf_id(const struct io_uring_cqe* cqe) {
    return (uint16_t)(cqe->flags >> IORING_CQE_BUFFER_SHIFT);
}

[[nodiscard]]
bool khr_uring_init(khr_uring_t* ring, const khr_uring_config_t* cfg);

void khr_uring_destroy(khr_uring_t* ring);

[[nodiscard]]
struct io_uring_sqe* khr_uring_get_sqe(khr_uring_t* ring);

[[nodiscard]]
int khr_uring_submit(khr_uring_t* ring, uint32_t min_complete);

void khr_uring_tick(khr_uring_t* ring);

[[nodiscard]]
bool khr_uring_peek_cqe(khr_uring_t* ring, struct io_uring_cqe** out_cqe);

void khr_uring_cqe_seen(khr_uring_t* ring, struct io_uring_cqe* cqe);

[[nodiscard]]
bool khr_uring_wait_cqe_timeout(khr_uring_t* ring, struct io_uring_cqe** out_cqe,
                                uint32_t timeout_ms);

[[nodiscard]]
bool khr_uring_register_clock(khr_uring_t* ring, int clockid);

[[nodiscard]]
bool khr_uring_register_buffers(khr_uring_t* ring, const struct iovec* iovs, uint32_t n);

[[nodiscard]]
bool khr_uring_unregister_buffers(khr_uring_t* ring);

[[nodiscard]]
bool khr_uring_register_files(khr_uring_t* ring, const int* fds, uint32_t n);

[[nodiscard]]
bool khr_uring_register_files_sparse(khr_uring_t* ring, uint32_t nr);

[[nodiscard]]
bool khr_uring_unregister_files(khr_uring_t* ring);

[[nodiscard]]
bool khr_uring_register_files_update(khr_uring_t* ring, uint32_t offset,
                                     const int* fds, uint32_t nr);

[[nodiscard]]
bool khr_uring_register_iowq_aff(khr_uring_t* ring, int cpu);

void khr_uring_enable_no_iowait(khr_uring_t* ring);

[[nodiscard]]
struct io_uring_sqe* khr_uring_prep_nop(khr_uring_t* ring, uint64_t user_data);

[[nodiscard]]
struct io_uring_sqe* khr_uring_prep_msg_ring(khr_uring_t* ring, int dst_ring_fd,
                                             uint64_t payload, uint32_t res);

[[nodiscard]]
struct io_uring_sqe* khr_uring_prep_socket(khr_uring_t* ring, int domain,
                                           int type, int protocol,
                                           uint32_t direct_slot, bool is_direct,
                                           uint64_t user_data);

[[nodiscard]]
struct io_uring_sqe* khr_uring_prep_connect(khr_uring_t* ring, int fd,
                                            const struct sockaddr* addr,
                                            socklen_t addrlen, bool is_direct,
                                            uint64_t user_data);

[[nodiscard]]
struct io_uring_sqe* khr_uring_prep_accept(khr_uring_t* ring, int listen_fd,
                                           struct sockaddr* addr, socklen_t* addrlen,
                                           int flags, uint32_t direct_slot,
                                           bool is_direct, uint64_t user_data);

[[nodiscard]]
struct io_uring_sqe* khr_uring_prep_close(khr_uring_t* ring, int fd,
                                          bool is_direct, uint64_t user_data);

[[nodiscard]]
struct io_uring_sqe* khr_uring_prep_openat2(khr_uring_t* ring, int dfd,
                                            const char* path,
                                            const void* how,
                                            uint32_t direct_slot, bool is_direct,
                                            uint64_t user_data);

[[nodiscard]]
struct io_uring_sqe* khr_uring_prep_read(khr_uring_t* ring, int fd,
                                         void* dst, uint32_t len,
                                         uint64_t offset, bool is_direct,
                                         uint64_t user_data);

[[nodiscard]]
struct io_uring_sqe* khr_uring_prep_write(khr_uring_t* ring, int fd,
                                          const void* src, uint32_t len,
                                          uint64_t offset, bool is_direct,
                                          uint64_t user_data);

[[nodiscard]]
struct io_uring_sqe* khr_uring_prep_read_fixed(khr_uring_t* ring, int fd,
                                               void* dst, uint32_t len,
                                               uint64_t offset, uint16_t buf_index,
                                               bool is_direct, uint64_t user_data);

[[nodiscard]]
struct io_uring_sqe* khr_uring_prep_send(khr_uring_t* ring, int fd,
                                         const void* buf, size_t len,
                                         int flags, bool is_direct,
                                         uint64_t user_data);

[[nodiscard]]
struct io_uring_sqe* khr_uring_prep_recv(khr_uring_t* ring, int fd,
                                         void* buf, size_t len,
                                         int flags, bool is_direct,
                                         uint64_t user_data);

[[nodiscard]]
struct io_uring_sqe* khr_uring_prep_recvmsg(khr_uring_t* ring, int fd,
                                            struct msghdr* msg, uint16_t bgid,
                                            bool multishot, uint64_t user_data);

[[nodiscard]]
struct io_uring_sqe* khr_uring_prep_sendmsg(khr_uring_t* ring, int fd,
                                            struct msghdr* msg, bool skip_success,
                                            uint64_t user_data);

[[nodiscard]]
struct io_uring_sqe* khr_uring_prep_eventfd_watch(khr_uring_t* ring, int efd,
                                                  uint64_t user_data);

[[nodiscard]]
struct io_uring_sqe* khr_uring_prep_fixed_fd_install(khr_uring_t* ring,
                                                     uint32_t fixed_index,
                                                     uint64_t user_data);

[[nodiscard]]
struct io_uring_sqe* khr_uring_prep_timeout(khr_uring_t* ring,
                                            struct __kernel_timespec* ts,
                                            uint64_t user_data);

#endif /* KHOROS_URING_RING_H */

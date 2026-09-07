#include "khoros/uring/ring.h"
#include "khoros/core/cpu.h"
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <sched.h>
#include <errno.h>
#include <linux/openat2.h>

[[nodiscard]]
bool khr_uring_init(khr_uring_t* ring, const khr_uring_config_t* cfg) {
    if (ring == nullptr) {
        return false;
    }
    *ring = (khr_uring_t){ .ring_fd = -1 };

    uint32_t sq_entries = (cfg != nullptr && cfg->sq_entries > 0) ? cfg->sq_entries : KHR_RING_A_SQ_ENTRIES;
    uint32_t cq_entries = (cfg != nullptr && cfg->cq_entries > 0) ? cfg->cq_entries : KHR_RING_A_CQ_ENTRIES;
    uint32_t flags = (cfg != nullptr && cfg->flags > 0) ? cfg->flags : (
        IORING_SETUP_SINGLE_ISSUER |
        IORING_SETUP_DEFER_TASKRUN |
        IORING_SETUP_COOP_TASKRUN  |
        IORING_SETUP_NO_SQARRAY    |
        IORING_SETUP_CLAMP         |
        IORING_SETUP_CQSIZE
    );

    struct io_uring_params params = {
        .flags = flags,
        .cq_entries = cq_entries,
    };

    int fd = khr_sys_io_uring_setup(sq_entries, &params);
    if (fd < 0) {
        return false;
    }

    ring->ring_fd = fd;
    ring->enter_fd = (uint32_t)fd;
    ring->enter_flags = 0;
    ring->features = params.features;
    ring->setup_flags = params.flags;
    ring->no_sqarray = (params.flags & IORING_SETUP_NO_SQARRAY) != 0;
    ring->clock_monotonic = false;

    size_t sq_sz = params.sq_off.array + params.sq_entries * sizeof(uint32_t);
    size_t cq_sz = params.cq_off.cqes + params.cq_entries * sizeof(struct io_uring_cqe);
    size_t ring_sz = (sq_sz > cq_sz) ? sq_sz : cq_sz;

    uint8_t* ring_ptr = (uint8_t*)mmap(nullptr, ring_sz, PROT_READ | PROT_WRITE,
                                       MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
    if (ring_ptr == MAP_FAILED) {
        close(fd);
        ring->ring_fd = -1;
        return false;
    }

    size_t sqes_sz = params.sq_entries * sizeof(struct io_uring_sqe);
    struct io_uring_sqe* sqes = (struct io_uring_sqe*)mmap(nullptr, sqes_sz,
                                                           PROT_READ | PROT_WRITE,
                                                           MAP_SHARED | MAP_POPULATE,
                                                           fd, IORING_OFF_SQES);
    if (sqes == MAP_FAILED) {
        munmap(ring_ptr, ring_sz);
        close(fd);
        ring->ring_fd = -1;
        return false;
    }

    ring->ring_sz = ring_sz;
    ring->sqes_sz = sqes_sz;
    ring->ring_ptr = ring_ptr;
    ring->sqes = sqes;

    /* Setup SQ pointers */
    ring->sq_khead = (_Atomic uint32_t*)(ring_ptr + params.sq_off.head);
    ring->sq_ktail = (_Atomic uint32_t*)(ring_ptr + params.sq_off.tail);
    ring->sq_kflags = (uint32_t*)(ring_ptr + params.sq_off.flags);
    ring->sq_karray = (uint32_t*)(ring_ptr + params.sq_off.array);
    ring->sq_mask = *(uint32_t*)(ring_ptr + params.sq_off.ring_mask);
    ring->sq_entries = *(uint32_t*)(ring_ptr + params.sq_off.ring_entries);
    ring->sq_tail = atomic_load_explicit(ring->sq_ktail, memory_order_relaxed);

    /* Setup CQ pointers */
    ring->cq_khead = (_Atomic uint32_t*)(ring_ptr + params.cq_off.head);
    ring->cq_ktail = (_Atomic uint32_t*)(ring_ptr + params.cq_off.tail);
    ring->cq_kflags = (uint32_t*)(ring_ptr + params.cq_off.flags);
    ring->cq_koverflow = (uint32_t*)(ring_ptr + params.cq_off.overflow);
    ring->cq_cqes = (struct io_uring_cqe*)(ring_ptr + params.cq_off.cqes);
    ring->cq_mask = *(uint32_t*)(ring_ptr + params.cq_off.ring_mask);
    ring->cq_entries = *(uint32_t*)(ring_ptr + params.cq_off.ring_entries);
    ring->cq_head = atomic_load_explicit(ring->cq_khead, memory_order_relaxed);

    /* Register ring FD if requested (offset ~0U lets the kernel pick a free slot). */
    if (cfg == nullptr || cfg->register_ring_fd) {
        struct io_uring_rsrc_update up = {
            .data = (uint64_t)fd,
            .offset = (uint32_t)-1,
        };
        int reg_ret = khr_sys_io_uring_register(fd, IORING_REGISTER_RING_FDS, &up, 1);
        if (reg_ret == 1) {
            ring->enter_fd = up.offset;
            ring->enter_flags |= IORING_ENTER_REGISTERED_RING;
        }
    }

    /* Register direct descriptor sparse file table if configured */
    ring->files_registered = false;
    ring->registered_files_count = 0;
    uint32_t files_nr = (cfg != nullptr) ? cfg->registered_files_nr : KHR_DEFAULT_SPARSE_FILES;
    if (files_nr > 0) {
        (void)khr_uring_register_files_sparse(ring, files_nr);
    }

    return true;
}

void khr_uring_destroy(khr_uring_t* ring) {
    if (ring == nullptr || ring->ring_fd < 0) {
        return;
    }
    if (ring->files_registered) {
        (void)khr_uring_unregister_files(ring);
    }
    if ((ring->enter_flags & IORING_ENTER_REGISTERED_RING) != 0) {
        struct io_uring_rsrc_update up = {
            .offset = ring->enter_fd,
        };
        (void)khr_sys_io_uring_register(ring->ring_fd, IORING_UNREGISTER_RING_FDS, &up, 1);
        ring->enter_flags &= (uint32_t)~IORING_ENTER_REGISTERED_RING;
        ring->enter_fd = (uint32_t)ring->ring_fd;
    }
    if (ring->sqes != nullptr) {
        munmap(ring->sqes, ring->sqes_sz);
        ring->sqes = nullptr;
    }
    if (ring->ring_ptr != nullptr) {
        munmap(ring->ring_ptr, ring->ring_sz);
        ring->ring_ptr = nullptr;
    }
    close(ring->ring_fd);
    ring->ring_fd = -1;
}

[[nodiscard]]
struct io_uring_sqe* khr_uring_get_sqe(khr_uring_t* ring) {
    if (ring == nullptr) {
        return nullptr;
    }
    uint32_t head = atomic_load_explicit(ring->sq_khead, memory_order_acquire);
    uint32_t tail = ring->sq_tail;

    if ((tail - head) >= ring->sq_entries) {
        return nullptr; /* Submission queue full */
    }

    uint32_t idx = tail & ring->sq_mask;
    if (!ring->no_sqarray && ring->sq_karray != nullptr) {
        ring->sq_karray[idx] = idx;
    }
    struct io_uring_sqe* sqe = &ring->sqes[idx];
    memset(sqe, 0, sizeof(*sqe));
    ring->sq_tail = tail + 1;
    return sqe;
}

[[nodiscard]]
int khr_uring_submit(khr_uring_t* ring, uint32_t min_complete) {
    if (ring == nullptr || ring->ring_fd < 0) {
        return -1;
    }
    uint32_t submitted = ring->sq_tail - atomic_load_explicit(ring->sq_ktail, memory_order_relaxed);

    /* Release barrier: ensure all SQEs are completely written before updating kernel tail */
    atomic_store_explicit(ring->sq_ktail, ring->sq_tail, memory_order_release);

    uint32_t flags = ring->enter_flags;
    if (min_complete > 0) {
        flags |= IORING_ENTER_GETEVENTS;
    } else if ((ring->setup_flags & IORING_SETUP_DEFER_TASKRUN) != 0) {
        /* Flush SQEs without sleeping. */
        flags |= IORING_ENTER_GETEVENTS;
    }

    return khr_sys_io_uring_enter(ring->enter_fd, submitted, min_complete, flags, nullptr);
}

[[nodiscard]]
bool khr_uring_peek_cqe(khr_uring_t* ring, struct io_uring_cqe** out_cqe) {
    if (ring == nullptr || out_cqe == nullptr) {
        return false;
    }
    uint32_t tail = atomic_load_explicit(ring->cq_ktail, memory_order_acquire);
    uint32_t head = ring->cq_head;

    if (head == tail) {
        *out_cqe = nullptr;
        return false;
    }

    *out_cqe = &ring->cq_cqes[head & ring->cq_mask];
    return true;
}

void khr_uring_cqe_seen(khr_uring_t* ring, [[maybe_unused]] struct io_uring_cqe* cqe) {
    if (ring == nullptr) {
        return;
    }
    ring->cq_head++;
    atomic_store_explicit(ring->cq_khead, ring->cq_head, memory_order_release);
}

void khr_uring_tick(khr_uring_t* ring) {
    if (ring == nullptr || ring->ring_fd < 0 || ring->sq_kflags == nullptr) {
        return;
    }
    uint32_t f = *ring->sq_kflags;
    if ((f & (IORING_SQ_TASKRUN | IORING_SQ_CQ_OVERFLOW)) != 0) {
        (void)khr_uring_submit(ring, 0);
    }
}

[[nodiscard]]
static uint32_t khr_uring_publish_sq(khr_uring_t* ring) {
    uint32_t submitted = ring->sq_tail - atomic_load_explicit(ring->sq_ktail, memory_order_relaxed);
    atomic_store_explicit(ring->sq_ktail, ring->sq_tail, memory_order_release);
    return submitted;
}

[[nodiscard]]
bool khr_uring_wait_cqe_timeout(khr_uring_t* ring, struct io_uring_cqe** out_cqe,
                                uint32_t timeout_ms) {
    if (ring == nullptr || out_cqe == nullptr) {
        return false;
    }

    khr_uring_tick(ring);
    if (khr_uring_peek_cqe(ring, out_cqe)) {
        return true;
    }

    uint32_t submitted = khr_uring_publish_sq(ring);

    /* Native kernel timed wait: argsz MUST be sizeof(io_uring_getevents_arg),
     * not _NSIG/8. That mismatch is why EXT_ARG previously returned -EINVAL
     * and forced a 50 µs nanosleep fallback. */
    if ((ring->features & IORING_FEAT_EXT_ARG) != 0 && timeout_ms > 0) {
        struct __kernel_timespec ts = {
            .tv_sec = (int64_t)(timeout_ms / 1000U),
            .tv_nsec = (long long)(timeout_ms % 1000U) * 1'000'000LL,
        };
        if (ts.tv_sec == 0 && ts.tv_nsec == 0) {
            ts.tv_nsec = 1;
        }
        struct io_uring_getevents_arg arg = {
            .ts = (uint64_t)&ts,
        };
        uint32_t flags = ring->enter_flags | IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG;
        int ret = khr_sys_io_uring_enter2(ring->enter_fd, submitted, 1, flags,
                                          &arg, sizeof(arg));
        if (ret >= 0 || khr_uring_peek_cqe(ring, out_cqe)) {
            return khr_uring_peek_cqe(ring, out_cqe);
        }
        if (errno == ETIME) {
            return false;
        }
        if (errno != EINVAL && errno != ENOSYS && errno != EOPNOTSUPP && errno != EINTR) {
            return false;
        }
        /* Fall through to PAUSE spin only if EXT_ARG is unsupported on older kernel or EINTR */
    } else if (submitted > 0 || (ring->setup_flags & IORING_SETUP_DEFER_TASKRUN) != 0) {
        uint32_t flags = ring->enter_flags | IORING_ENTER_GETEVENTS;
        (void)khr_sys_io_uring_enter(ring->enter_fd, submitted, 0, flags, nullptr);
        if (khr_uring_peek_cqe(ring, out_cqe)) {
            return true;
        }
    }

    struct timespec start = {};
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        khr_uring_tick(ring);
        if (khr_uring_peek_cqe(ring, out_cqe)) {
            return true;
        }
        submitted = khr_uring_publish_sq(ring);
        uint32_t flags = ring->enter_flags | IORING_ENTER_GETEVENTS;
        (void)khr_sys_io_uring_enter(ring->enter_fd, submitted, 0, flags, nullptr);
        if (khr_uring_peek_cqe(ring, out_cqe)) {
            return true;
        }

        struct timespec now = {};
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t elapsed_ns = (now.tv_sec - start.tv_sec) * 1'000'000'000LL +
                             (now.tv_nsec - start.tv_nsec);
        if (elapsed_ns >= (int64_t)timeout_ms * 1'000'000LL) {
            return false;
        }
        for (uint32_t i = 0; i < 64; i++) {
            khr_cpu_pause();
        }
    }
}

[[nodiscard]]
bool khr_uring_register_clock(khr_uring_t* ring, int clockid) {
    if (ring == nullptr || ring->ring_fd < 0) {
        return false;
    }
    struct io_uring_clock_register clk = {
        .clockid = (uint32_t)clockid,
    };
    /* Kernel 7.2 rejects a non-zero nr_args for IORING_REGISTER_CLOCK. */
    int r = khr_sys_io_uring_register(ring->ring_fd, IORING_REGISTER_CLOCK, &clk, 0);
    if (r < 0) {
        return false;
    }
    ring->clock_monotonic = (clockid == CLOCK_MONOTONIC);
    return true;
}

[[nodiscard]]
bool khr_uring_register_buffers(khr_uring_t* ring, const struct iovec* iovs, uint32_t n) {
    if (ring == nullptr || ring->ring_fd < 0 || iovs == nullptr || n == 0) {
        return false;
    }
    constexpr int MAX_RETRIES = 100;
    for (int retry = 0; retry < MAX_RETRIES; retry++) {
        if (khr_sys_io_uring_register(ring->ring_fd, IORING_REGISTER_BUFFERS, iovs, n) == 0) {
            return true;
        }
        if (errno == ENOMEM || errno == EAGAIN || errno == EBUSY) {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 1'000'000 };
            nanosleep(&ts, nullptr);
        } else {
            break;
        }
    }
    return false;
}

[[nodiscard]]
bool khr_uring_unregister_buffers(khr_uring_t* ring) {
    if (ring == nullptr || ring->ring_fd < 0) {
        return false;
    }
    return khr_sys_io_uring_register(ring->ring_fd, IORING_UNREGISTER_BUFFERS, nullptr, 0) == 0;
}

[[nodiscard]]
bool khr_uring_register_files(khr_uring_t* ring, const int* fds, uint32_t n) {
    if (ring == nullptr || ring->ring_fd < 0 || fds == nullptr || n == 0) {
        return false;
    }
    if (ring->files_registered) {
        (void)khr_uring_unregister_files(ring);
    }
    int r = khr_sys_io_uring_register(ring->ring_fd, IORING_REGISTER_FILES, fds, n);
    if (r == 0) {
        ring->files_registered = true;
        ring->registered_files_count = n;
        return true;
    }
    return false;
}

[[nodiscard]]
bool khr_uring_register_files_sparse(khr_uring_t* ring, uint32_t nr) {
    if (ring == nullptr || ring->ring_fd < 0 || nr == 0) {
        return false;
    }
    if (ring->files_registered) {
        (void)khr_uring_unregister_files(ring);
    }
    struct io_uring_rsrc_register rr = {
        .nr = nr,
        .flags = IORING_RSRC_REGISTER_SPARSE,
    };
    int r = khr_sys_io_uring_register(ring->ring_fd, IORING_REGISTER_FILES2, &rr, sizeof(rr));
    if (r == 0) {
        ring->files_registered = true;
        ring->registered_files_count = nr;
        return true;
    }
    /* Fallback: allocate array of -1 */
    int* fds = (int*)mmap(nullptr, (size_t)nr * sizeof(int), PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (fds == MAP_FAILED) {
        return false;
    }
    for (uint32_t i = 0; i < nr; i++) {
        fds[i] = -1;
    }
    r = khr_sys_io_uring_register(ring->ring_fd, IORING_REGISTER_FILES, fds, nr);
    munmap(fds, (size_t)nr * sizeof(int));
    if (r == 0) {
        ring->files_registered = true;
        ring->registered_files_count = nr;
        return true;
    }
    return false;
}

[[nodiscard]]
bool khr_uring_unregister_files(khr_uring_t* ring) {
    if (ring == nullptr || ring->ring_fd < 0) {
        return false;
    }
    int r = khr_sys_io_uring_register(ring->ring_fd, IORING_UNREGISTER_FILES, nullptr, 0);
    if (r == 0) {
        ring->files_registered = false;
        ring->registered_files_count = 0;
        return true;
    }
    return false;
}

[[nodiscard]]
bool khr_uring_register_files_update(khr_uring_t* ring, uint32_t offset,
                                     const int* fds, uint32_t nr) {
    if (ring == nullptr || ring->ring_fd < 0 || fds == nullptr || nr == 0) {
        return false;
    }
    struct io_uring_files_update up = {
        .offset = offset,
        .resv = 0,
        .fds = (uint64_t)fds,
    };
    int r = khr_sys_io_uring_register(ring->ring_fd, IORING_REGISTER_FILES_UPDATE, &up, nr);
    return r == (int)nr;
}

[[nodiscard]]
bool khr_uring_register_iowq_aff(khr_uring_t* ring, int cpu) {
    if (ring == nullptr || ring->ring_fd < 0 || cpu < 0) {
        return false;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET((unsigned)cpu, &set);
    return khr_sys_io_uring_register(ring->ring_fd, IORING_REGISTER_IOWQ_AFF, &set, sizeof(set)) == 0;
}

void khr_uring_enable_no_iowait(khr_uring_t* ring) {
    if (ring == nullptr) {
        return;
    }
    if ((ring->features & IORING_FEAT_NO_IOWAIT) != 0) {
        ring->enter_flags |= IORING_ENTER_NO_IOWAIT;
    }
}

[[nodiscard]]
struct io_uring_sqe* khr_uring_prep_nop(khr_uring_t* ring, uint64_t user_data) {
    struct io_uring_sqe* sqe = khr_uring_get_sqe(ring);
    if (sqe == nullptr) {
        return nullptr;
    }
    sqe->opcode = IORING_OP_NOP;
    sqe->user_data = user_data;
    return sqe;
}

[[nodiscard]]
struct io_uring_sqe* khr_uring_prep_msg_ring(khr_uring_t* ring, int dst_ring_fd,
                                             uint64_t payload, uint32_t res) {
    struct io_uring_sqe* sqe = khr_uring_get_sqe(ring);
    if (sqe == nullptr) {
        return nullptr;
    }
    sqe->opcode = IORING_OP_MSG_RING;
    sqe->fd = dst_ring_fd;
    sqe->addr = IORING_MSG_DATA;
    sqe->off = payload;
    sqe->len = res;
    sqe->flags = IOSQE_CQE_SKIP_SUCCESS;
    sqe->user_data = KHR_TAG_MSG_RING;
    return sqe;
}

[[nodiscard]]
struct io_uring_sqe* khr_uring_prep_socket(khr_uring_t* ring, int domain,
                                           int type, int protocol,
                                           uint32_t direct_slot, bool is_direct,
                                           uint64_t user_data) {
    struct io_uring_sqe* sqe = khr_uring_get_sqe(ring);
    if (sqe == nullptr) {
        return nullptr;
    }
    sqe->opcode = IORING_OP_SOCKET;
    sqe->fd = domain;
    sqe->off = (uint64_t)type;
    sqe->len = (uint32_t)protocol;
    sqe->user_data = user_data;
    if (is_direct) {
        /* Direct descriptors are private to the ring; avoid SOCK_CLOEXEC which is rejected on fixed files */
        sqe->off = (uint64_t)(type & ~SOCK_CLOEXEC);
        sqe->file_index = direct_slot + 1U;
    }
    return sqe;
}

[[nodiscard]]
struct io_uring_sqe* khr_uring_prep_connect(khr_uring_t* ring, int fd,
                                            const struct sockaddr* addr,
                                            socklen_t addrlen, bool is_direct,
                                            uint64_t user_data) {
    struct io_uring_sqe* sqe = khr_uring_get_sqe(ring);
    if (sqe == nullptr) {
        return nullptr;
    }
    sqe->opcode = IORING_OP_CONNECT;
    sqe->fd = fd;
    sqe->addr = (uint64_t)addr;
    sqe->off = (uint64_t)addrlen;
    sqe->user_data = user_data;
    if (is_direct) {
        sqe->flags |= IOSQE_FIXED_FILE;
    }
    return sqe;
}

[[nodiscard]]
struct io_uring_sqe* khr_uring_prep_accept(khr_uring_t* ring, int listen_fd,
                                           struct sockaddr* addr, socklen_t* addrlen,
                                           int flags, uint32_t direct_slot,
                                           bool is_direct, uint64_t user_data) {
    struct io_uring_sqe* sqe = khr_uring_get_sqe(ring);
    if (sqe == nullptr) {
        return nullptr;
    }
    sqe->opcode = IORING_OP_ACCEPT;
    sqe->fd = listen_fd;
    sqe->addr = (uint64_t)addr;
    sqe->addr2 = (uint64_t)addrlen;
    sqe->accept_flags = (uint32_t)flags;
    sqe->user_data = user_data;
    if (is_direct) {
        sqe->file_index = direct_slot + 1U;
    }
    return sqe;
}

[[nodiscard]]
struct io_uring_sqe* khr_uring_prep_close(khr_uring_t* ring, int fd,
                                          bool is_direct, uint64_t user_data) {
    struct io_uring_sqe* sqe = khr_uring_get_sqe(ring);
    if (sqe == nullptr) {
        return nullptr;
    }
    sqe->opcode = IORING_OP_CLOSE;
    if (is_direct) {
        sqe->fd = 0;
        sqe->file_index = (uint32_t)fd + 1U;
    } else {
        sqe->fd = fd;
        sqe->file_index = 0;
    }
    sqe->user_data = user_data;
    return sqe;
}

[[nodiscard]]
struct io_uring_sqe* khr_uring_prep_openat2(khr_uring_t* ring, int dfd,
                                            const char* path,
                                            const void* how,
                                            uint32_t direct_slot, bool is_direct,
                                            uint64_t user_data) {
    struct io_uring_sqe* sqe = khr_uring_get_sqe(ring);
    if (sqe == nullptr) {
        return nullptr;
    }
    sqe->opcode = IORING_OP_OPENAT2;
    sqe->fd = dfd;
    sqe->addr = (uint64_t)path;
    sqe->len = (uint32_t)sizeof(struct open_how);
    sqe->off = (uint64_t)how;
    sqe->user_data = user_data;
    if (is_direct) {
        sqe->file_index = direct_slot + 1U;
    }
    return sqe;
}

[[nodiscard]]
struct io_uring_sqe* khr_uring_prep_read(khr_uring_t* ring, int fd,
                                         void* dst, uint32_t len,
                                         uint64_t offset, bool is_direct,
                                         uint64_t user_data) {
    struct io_uring_sqe* sqe = khr_uring_get_sqe(ring);
    if (sqe == nullptr) {
        return nullptr;
    }
    sqe->opcode = IORING_OP_READ;
    sqe->fd = fd;
    sqe->addr = (uint64_t)dst;
    sqe->len = len;
    sqe->off = offset;
    sqe->user_data = user_data;
    if (is_direct) {
        sqe->flags |= IOSQE_FIXED_FILE;
    }
    return sqe;
}

[[nodiscard]]
struct io_uring_sqe* khr_uring_prep_write(khr_uring_t* ring, int fd,
                                          const void* src, uint32_t len,
                                          uint64_t offset, bool is_direct,
                                          uint64_t user_data) {
    struct io_uring_sqe* sqe = khr_uring_get_sqe(ring);
    if (sqe == nullptr) {
        return nullptr;
    }
    sqe->opcode = IORING_OP_WRITE;
    sqe->fd = fd;
    sqe->addr = (uint64_t)src;
    sqe->len = len;
    sqe->off = offset;
    sqe->user_data = user_data;
    if (is_direct) {
        sqe->flags |= IOSQE_FIXED_FILE;
    }
    return sqe;
}

[[nodiscard]]
struct io_uring_sqe* khr_uring_prep_read_fixed(khr_uring_t* ring, int fd,
                                               void* dst, uint32_t len,
                                               uint64_t offset, uint16_t buf_index,
                                               bool is_direct, uint64_t user_data) {
    struct io_uring_sqe* sqe = khr_uring_get_sqe(ring);
    if (sqe == nullptr) {
        return nullptr;
    }
    sqe->opcode = IORING_OP_READ_FIXED;
    sqe->fd = fd;
    sqe->addr = (uint64_t)dst;
    sqe->len = len;
    sqe->off = offset;
    sqe->buf_index = buf_index;
    sqe->user_data = user_data;
    if (is_direct) {
        sqe->flags |= IOSQE_FIXED_FILE;
    }
    return sqe;
}

[[nodiscard]]
struct io_uring_sqe* khr_uring_prep_send(khr_uring_t* ring, int fd,
                                         const void* buf, size_t len,
                                         int flags, bool is_direct,
                                         uint64_t user_data) {
    struct io_uring_sqe* sqe = khr_uring_get_sqe(ring);
    if (sqe == nullptr) {
        return nullptr;
    }
    sqe->opcode = IORING_OP_SEND;
    sqe->fd = fd;
    sqe->addr = (uint64_t)buf;
    sqe->len = (uint32_t)len;
    sqe->msg_flags = (uint32_t)flags;
    sqe->user_data = user_data;
    if (is_direct) {
        sqe->flags |= IOSQE_FIXED_FILE;
    }
    return sqe;
}

[[nodiscard]]
struct io_uring_sqe* khr_uring_prep_recv(khr_uring_t* ring, int fd,
                                         void* buf, size_t len,
                                         int flags, bool is_direct,
                                         uint64_t user_data) {
    struct io_uring_sqe* sqe = khr_uring_get_sqe(ring);
    if (sqe == nullptr) {
        return nullptr;
    }
    sqe->opcode = IORING_OP_RECV;
    sqe->fd = fd;
    sqe->addr = (uint64_t)buf;
    sqe->len = (uint32_t)len;
    sqe->msg_flags = (uint32_t)flags;
    sqe->user_data = user_data;
    if (is_direct) {
        sqe->flags |= IOSQE_FIXED_FILE;
    }
    return sqe;
}

[[nodiscard]]
struct io_uring_sqe* khr_uring_prep_recvmsg(khr_uring_t* ring, int fd,
                                            struct msghdr* msg, uint16_t bgid,
                                            bool multishot, uint64_t user_data) {
    struct io_uring_sqe* sqe = khr_uring_get_sqe(ring);
    if (sqe == nullptr) {
        return nullptr;
    }
    sqe->opcode = IORING_OP_RECVMSG;
    sqe->fd = fd;
    sqe->addr = (uint64_t)msg;
    sqe->msg_flags = 0;
    sqe->user_data = user_data;
    if (multishot) {
        sqe->ioprio = IORING_RECV_MULTISHOT;
    }
    if (bgid != 0xFFFF) {
        sqe->flags |= IOSQE_BUFFER_SELECT;
        sqe->buf_group = bgid;
    }
    return sqe;
}

[[nodiscard]]
struct io_uring_sqe* khr_uring_prep_sendmsg(khr_uring_t* ring, int fd,
                                            struct msghdr* msg, bool skip_success,
                                            uint64_t user_data) {
    struct io_uring_sqe* sqe = khr_uring_get_sqe(ring);
    if (sqe == nullptr) {
        return nullptr;
    }
    sqe->opcode = IORING_OP_SENDMSG;
    sqe->fd = fd;
    sqe->addr = (uint64_t)msg;
    sqe->user_data = user_data;
    if (skip_success) {
        sqe->flags = IOSQE_CQE_SKIP_SUCCESS;
    }
    return sqe;
}

[[nodiscard]]
struct io_uring_sqe* khr_uring_prep_eventfd_watch(khr_uring_t* ring, int efd,
                                                  uint64_t user_data) {
    struct io_uring_sqe* sqe = khr_uring_get_sqe(ring);
    if (sqe == nullptr) {
        return nullptr;
    }
    sqe->opcode = IORING_OP_POLL_ADD;
    sqe->fd = efd;
    sqe->poll32_events = KHR_POLLIN;
    sqe->user_data = user_data;
    return sqe;
}

[[nodiscard]]
struct io_uring_sqe* khr_uring_prep_fixed_fd_install(khr_uring_t* ring,
                                                     uint32_t fixed_index,
                                                     uint64_t user_data) {
    struct io_uring_sqe* sqe = khr_uring_get_sqe(ring);
    if (sqe == nullptr) {
        return nullptr;
    }
    sqe->opcode = IORING_OP_FIXED_FD_INSTALL;
    sqe->fd = (int32_t)fixed_index;
    sqe->flags = IOSQE_FIXED_FILE;
    sqe->user_data = user_data;
    return sqe;
}

[[nodiscard]]
struct io_uring_sqe* khr_uring_prep_timeout(khr_uring_t* ring,
                                            struct __kernel_timespec* ts,
                                            uint64_t user_data) {
    struct io_uring_sqe* sqe = khr_uring_get_sqe(ring);
    if (sqe == nullptr) {
        return nullptr;
    }
    sqe->opcode = IORING_OP_TIMEOUT;
    sqe->addr = (uint64_t)ts;
    sqe->len = 1;
    sqe->user_data = user_data;
    return sqe;
}

#define _GNU_SOURCE
#include "uring/ring.h"

#include <errno.h>
#include <linux/time_types.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

static int sys_setup(unsigned entries, struct io_uring_params *p)
{
    return (int)syscall(__NR_io_uring_setup, entries, p);
}

static int sys_enter(int fd, unsigned to_submit, unsigned min_complete, unsigned flags,
                     const void *arg, size_t argsz)
{
    return (int)syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags, arg, argsz);
}

static int sys_register(int fd, unsigned opcode, const void *arg, unsigned nr)
{
    return (int)syscall(__NR_io_uring_register, fd, opcode, arg, nr);
}

static void unmap_rings(struct wp_uring *r)
{
    if (r->sqes_ptr && r->sqes_ptr != MAP_FAILED)
        munmap(r->sqes_ptr, r->sqes_sz);
    if (r->sq_ptr && r->sq_ptr != MAP_FAILED)
        munmap(r->sq_ptr, r->sq_map_sz);
    r->sqes_ptr = NULL;
    r->sq_ptr = NULL;
}

static int mmap_rings(struct wp_uring *r)
{
    const struct io_uring_params *p = &r->params;
    size_t sq_sz = (size_t)p->sq_off.array + (size_t)p->sq_entries * sizeof(unsigned);
    size_t cq_sz = (size_t)p->cq_off.cqes + (size_t)p->cq_entries * sizeof(struct io_uring_cqe);

    if (p->features & IORING_FEAT_SINGLE_MMAP) {
        if (cq_sz > sq_sz)
            sq_sz = cq_sz;
        r->sq_map_sz = sq_sz;
        r->sq_ptr = mmap(NULL, sq_sz, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_POPULATE, r->fd, (off_t)IORING_OFF_SQ_RING);
        if (r->sq_ptr == MAP_FAILED)
            return -errno;
    } else {
        return -ENOTSUP;
    }

    r->sqes_sz = (size_t)p->sq_entries * sizeof(struct io_uring_sqe);
    r->sqes_ptr = mmap(NULL, r->sqes_sz, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_POPULATE, r->fd, (off_t)IORING_OFF_SQES);
    if (r->sqes_ptr == MAP_FAILED) {
        int e = errno;
        munmap(r->sq_ptr, r->sq_map_sz);
        r->sq_ptr = NULL;
        return -e;
    }

    r->sq_head = (_Atomic unsigned *)((uint8_t *)r->sq_ptr + p->sq_off.head);
    r->sq_tail = (_Atomic unsigned *)((uint8_t *)r->sq_ptr + p->sq_off.tail);
    r->sq_mask = (unsigned *)((uint8_t *)r->sq_ptr + p->sq_off.ring_mask);
    r->sq_entries = (unsigned *)((uint8_t *)r->sq_ptr + p->sq_off.ring_entries);
    r->sq_flags = (unsigned *)((uint8_t *)r->sq_ptr + p->sq_off.flags);
    if (r->no_sqarray)
        r->sq_array = NULL;
    else
        r->sq_array = (unsigned *)((uint8_t *)r->sq_ptr + p->sq_off.array);

    r->cq_head = (_Atomic unsigned *)((uint8_t *)r->sq_ptr + p->cq_off.head);
    r->cq_tail = (_Atomic unsigned *)((uint8_t *)r->sq_ptr + p->cq_off.tail);
    r->cq_mask = (unsigned *)((uint8_t *)r->sq_ptr + p->cq_off.ring_mask);
    r->cq_entries = (unsigned *)((uint8_t *)r->sq_ptr + p->cq_off.ring_entries);
    r->cq_overflow = (unsigned *)((uint8_t *)r->sq_ptr + p->cq_off.overflow);
    r->cqes = (struct io_uring_cqe *)((uint8_t *)r->sq_ptr + p->cq_off.cqes);
    r->sq_prepared = atomic_load_explicit(r->sq_tail, memory_order_relaxed);
    return 0;
}

static int try_setup(struct wp_uring *r, unsigned sq_entries, unsigned flags)
{
    memset(r, 0, sizeof(*r));
    r->fd = -1;
    r->params.flags = flags;
    r->params.cq_entries = sq_entries * 4u;
    r->no_sqarray = (flags & IORING_SETUP_NO_SQARRAY) != 0;

    r->fd = sys_setup(sq_entries, &r->params);
    if (r->fd < 0)
        return -errno;
    return mmap_rings(r);
}

int wp_uring_setup(struct wp_uring *r, unsigned sq_entries)
{
    static const unsigned tries[] = {
        IORING_SETUP_SINGLE_ISSUER |
            IORING_SETUP_DEFER_TASKRUN |
            IORING_SETUP_COOP_TASKRUN |
            IORING_SETUP_SUBMIT_ALL |
            IORING_SETUP_CQSIZE |
            IORING_SETUP_CLAMP |
            IORING_SETUP_NO_SQARRAY,
        IORING_SETUP_SINGLE_ISSUER |
            IORING_SETUP_DEFER_TASKRUN |
            IORING_SETUP_SUBMIT_ALL |
            IORING_SETUP_CQSIZE |
            IORING_SETUP_CLAMP |
            IORING_SETUP_NO_SQARRAY,
        IORING_SETUP_SINGLE_ISSUER |
            IORING_SETUP_DEFER_TASKRUN |
            IORING_SETUP_SUBMIT_ALL |
            IORING_SETUP_CQSIZE |
            IORING_SETUP_CLAMP,
    };
    int last = -EINVAL;

    if (!r || sq_entries == 0)
        return -EINVAL;

    for (size_t i = 0; i < sizeof(tries) / sizeof(tries[0]); i++) {
        last = try_setup(r, sq_entries, tries[i]);
        if (last == 0) {
            r->enter_fd = r->fd;
            r->enter_flags = IORING_ENTER_GETEVENTS;
            r->ring_reg_index = -1;
            r->files_registered = 0;
            if (r->params.features & IORING_FEAT_REG_REG_RING)
                (void)wp_uring_register_ring_fd(r);
            if (r->params.features & IORING_FEAT_NO_IOWAIT)
                r->enter_flags |= IORING_ENTER_NO_IOWAIT;
            return 0;
        }
        if (r->fd >= 0) {
            unmap_rings(r);
            close(r->fd);
        }
        memset(r, 0, sizeof(*r));
        r->fd = -1;
    }
    return last;
}

int wp_uring_register(struct wp_uring *r, unsigned opcode, const void *arg, unsigned nr)
{
    if (!r || r->fd < 0)
        return -EBADF;
    if (sys_register(r->fd, opcode, arg, nr) < 0)
        return -errno;
    return 0;
}

int wp_uring_register_files(struct wp_uring *r, const int *fds, unsigned n)
{
    int ret;

    if (!fds || n == 0)
        return -EINVAL;
    ret = wp_uring_register(r, IORING_REGISTER_FILES, fds, n);
    if (ret == 0)
        r->files_registered = n;
    return ret;
}

int wp_uring_register_ring_fd(struct wp_uring *r)
{
    struct io_uring_rsrc_update up = {
        .offset = (uint32_t)-1,
        .data = (uint64_t)(unsigned)r->fd,
    };
    int ret;

    if (!r || r->fd < 0)
        return -EBADF;
    ret = wp_uring_register(r, IORING_REGISTER_RING_FDS, &up, 1);
    if (ret < 0)
        return ret;
    r->ring_reg_index = (int)up.offset;
    r->enter_fd = r->ring_reg_index;
    r->enter_flags |= IORING_ENTER_REGISTERED_RING;
    return 0;
}

void wp_uring_destroy(struct wp_uring *r)
{
    if (!r)
        return;
    if (r->fd >= 0) {
        if (r->files_registered)
            (void)sys_register(r->fd, IORING_UNREGISTER_FILES, NULL, 0);
        if (r->ring_reg_index >= 0) {
            struct io_uring_rsrc_update up = {
                .offset = (uint32_t)r->ring_reg_index,
            };
            (void)sys_register(r->fd, IORING_UNREGISTER_RING_FDS, &up, 1);
        }
    }
    unmap_rings(r);
    if (r->fd >= 0)
        close(r->fd);
    memset(r, 0, sizeof(*r));
    r->fd = -1;
}

static void feat(FILE *out, unsigned bits, unsigned bit, const char *name)
{
    fprintf(out, "  feat %-24s %s\n", name, (bits & bit) ? "yes" : "no");
}

static void setupf(FILE *out, unsigned bits, unsigned bit, const char *name)
{
    fprintf(out, "  setup %-24s %s\n", name, (bits & bit) ? "yes" : "no");
}

void wp_uring_log_caps(const struct wp_uring *r, FILE *out)
{
    unsigned f;
    unsigned s;
    struct {
        uint8_t last_op;
        uint8_t ops_len;
        uint16_t resv;
        uint32_t resv2[3];
        struct io_uring_probe_op ops[IORING_OP_LAST];
    } probe;
    const int want[] = {
        IORING_OP_NOP,
        IORING_OP_SENDMSG,
        IORING_OP_RECVMSG,
        IORING_OP_SENDMSG_ZC,
        IORING_OP_SOCKET,
        IORING_OP_CONNECT,
        IORING_OP_TIMEOUT,
        IORING_OP_POLL_ADD,
    };
    const char *names[] = {
        "NOP", "SENDMSG", "RECVMSG", "SENDMSG_ZC", "SOCKET", "CONNECT", "TIMEOUT", "POLL_ADD",
    };

    if (!r || !out)
        return;

    f = r->params.features;
    s = r->params.flags;
    fprintf(out, "uring fd=%d sq=%u cq=%u no_sqarray=%s\n",
            r->fd, r->params.sq_entries, r->params.cq_entries,
            r->no_sqarray ? "yes" : "no");
    setupf(out, s, IORING_SETUP_SINGLE_ISSUER, "SINGLE_ISSUER");
    setupf(out, s, IORING_SETUP_DEFER_TASKRUN, "DEFER_TASKRUN");
    setupf(out, s, IORING_SETUP_COOP_TASKRUN, "COOP_TASKRUN");
    setupf(out, s, IORING_SETUP_SUBMIT_ALL, "SUBMIT_ALL");
    setupf(out, s, IORING_SETUP_CQSIZE, "CQSIZE");
    setupf(out, s, IORING_SETUP_CLAMP, "CLAMP");
    setupf(out, s, IORING_SETUP_NO_SQARRAY, "NO_SQARRAY");
    setupf(out, s, IORING_SETUP_SQPOLL, "SQPOLL");
    fprintf(out, "  enter_fd=%d ring_reg_index=%d enter_flags=0x%x files=%u\n",
            r->enter_fd, r->ring_reg_index, r->enter_flags, r->files_registered);
    feat(out, f, IORING_FEAT_SINGLE_MMAP, "SINGLE_MMAP");
    feat(out, f, IORING_FEAT_NODROP, "NODROP");
    feat(out, f, IORING_FEAT_FAST_POLL, "FAST_POLL");
    feat(out, f, IORING_FEAT_EXT_ARG, "EXT_ARG");
    feat(out, f, IORING_FEAT_NATIVE_WORKERS, "NATIVE_WORKERS");
    feat(out, f, IORING_FEAT_CQE_SKIP, "CQE_SKIP");
    feat(out, f, IORING_FEAT_REG_REG_RING, "REG_REG_RING");
    feat(out, f, IORING_FEAT_RECVSEND_BUNDLE, "RECVSEND_BUNDLE");
    feat(out, f, IORING_FEAT_NO_IOWAIT, "NO_IOWAIT");

    memset(&probe, 0, sizeof(probe));
    if (sys_register(r->fd, IORING_REGISTER_PROBE, &probe, IORING_OP_LAST) < 0) {
        fprintf(out, "  probe: failed errno=%d (%s)\n", errno, strerror(errno));
        return;
    }
    fprintf(out, "  probe last_op=%u\n", probe.last_op);
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        int op = want[i];
        int ok = 0;
        if (op < probe.ops_len)
            ok = (probe.ops[op].flags & IO_URING_OP_SUPPORTED) != 0;
        fprintf(out, "  op %-12s %s\n", names[i], ok ? "yes" : "no");
    }
}

struct io_uring_sqe *wp_uring_get_sqe(struct wp_uring *r)
{
    unsigned head;
    unsigned next;
    unsigned mask;
    struct io_uring_sqe *sqes;
    struct io_uring_sqe *sqe;

    if (!r)
        return NULL;
    head = atomic_load_explicit(r->sq_head, memory_order_acquire);
    next = r->sq_prepared + 1;
    if (next - head > *r->sq_entries)
        return NULL;
    mask = *r->sq_mask;
    sqes = (struct io_uring_sqe *)r->sqes_ptr;
    sqe = &sqes[r->sq_prepared & mask];
    if (r->sq_array)
        r->sq_array[r->sq_prepared & mask] = r->sq_prepared & mask;
    r->sq_prepared = next;
    memset(sqe, 0, sizeof(*sqe));
    return sqe;
}

int wp_uring_submit(struct wp_uring *r, unsigned min_complete)
{
    return wp_uring_submit_timeout(r, min_complete, 0);
}

int wp_uring_submit_timeout(struct wp_uring *r, unsigned min_complete, uint64_t timeout_ns)
{
    unsigned tail;
    unsigned n;
    unsigned flags;
    int ret;

    if (!r || r->fd < 0)
        return -EBADF;
    tail = atomic_load_explicit(r->sq_tail, memory_order_relaxed);
    n = r->sq_prepared - tail;
    atomic_store_explicit(r->sq_tail, r->sq_prepared, memory_order_release);

    flags = r->enter_flags;
    if (timeout_ns > 0 && min_complete > 0 &&
        (r->params.features & IORING_FEAT_EXT_ARG)) {
        struct __kernel_timespec ts = {
            .tv_sec = (__kernel_time64_t)(timeout_ns / 1000000000ull),
            .tv_nsec = (__kernel_long_t)(timeout_ns % 1000000000ull),
        };
        struct io_uring_getevents_arg arg = {
            .ts = (uint64_t)(uintptr_t)&ts,
        };
        ret = sys_enter(r->enter_fd, n, min_complete, flags | IORING_ENTER_EXT_ARG,
                        &arg, sizeof(arg));
    } else {
        ret = sys_enter(r->enter_fd, n, min_complete, flags, NULL, 0);
    }
    if (ret < 0)
        return -errno;
    return ret;
}

unsigned wp_uring_cq_ready(const struct wp_uring *r)
{
    unsigned head;
    unsigned tail;

    if (!r)
        return 0;
    head = atomic_load_explicit(r->cq_head, memory_order_acquire);
    tail = atomic_load_explicit(r->cq_tail, memory_order_acquire);
    return tail - head;
}

struct io_uring_cqe *wp_uring_cqe_at(struct wp_uring *r, unsigned i)
{
    unsigned head;

    if (!r)
        return NULL;
    head = atomic_load_explicit(r->cq_head, memory_order_relaxed);
    return &r->cqes[(head + i) & *r->cq_mask];
}

void wp_uring_cq_advance(struct wp_uring *r, unsigned n)
{
    unsigned head;

    if (!r || n == 0)
        return;
    head = atomic_load_explicit(r->cq_head, memory_order_relaxed);
    atomic_store_explicit(r->cq_head, head + n, memory_order_release);
}

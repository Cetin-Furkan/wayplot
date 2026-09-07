# Raw io_uring (no liburing)

Khoros talks to the kernel with three syscalls only. `liburing` is banned. `<sys/epoll.h>` and `<poll.h>` are banned. `IORING_OP_POLL_ADD` is allowed (it is an io_uring opcode, not `poll(2)`).

## Files

| File | Role |
|---|---|
| `include/khoros/uring/raw_syscalls.h` | `khr_sys_io_uring_setup` / `enter` / `enter2` / `register` |
| `src/uring/raw_syscalls.c` | Direct `syscall(__NR_io_uring_*)` |
| `include/khoros/uring/ring.h` | Ring object, configs, SQE prep helpers |
| `src/uring/ring.c` | mmap, submit, CQ harvest, registrations |

## Syscall wrappers

```c
int khr_sys_io_uring_setup(uint32_t entries, struct io_uring_params* params);
int khr_sys_io_uring_enter(int fd, uint32_t to_submit, uint32_t min_complete,
                           uint32_t flags, sigset_t* sig);
int khr_sys_io_uring_enter2(int fd, uint32_t to_submit, uint32_t min_complete,
                            uint32_t flags, const void* arg, size_t argsz);
int khr_sys_io_uring_register(int fd, uint32_t opcode, const void* arg, uint32_t nr_args);
```

`enter` is `enter2` with a `sigset_t*` and `argsz = _NSIG / 8`. Use `enter2` when `IORING_ENTER_EXT_ARG` is set (`struct io_uring_getevents_arg`).

Always pass `ring->ring_fd` to `io_uring_register`. Use `ring->enter_fd` only for `io_uring_enter`, and only with `IORING_ENTER_REGISTERED_RING` if that fd is a registered index.

## Ring A vs Ring B

Configs are `khr_uring_config_ring_a()` and `khr_uring_config_ring_b()`.

| | Ring A (presentation) | Ring B (throughput) |
|---|---|---|
| SQ / CQ | 64 / 256 | 128 / 256 |
| Setup flags | `SINGLE_ISSUER \| DEFER_TASKRUN \| COOP_TASKRUN \| NO_SQARRAY \| CLAMP \| CQSIZE` | `SINGLE_ISSUER \| DEFER_TASKRUN \| NO_SQARRAY \| CQSIZE` |
| Clock | `IORING_REGISTER_CLOCK` (`CLOCK_MONOTONIC`) | none |
| Enter extras | `IORING_ENTER_NO_IOWAIT` if `IORING_FEAT_NO_IOWAIT` | none |
| Buffers | PBUF rings (see [pbuf.md](pbuf.md)) | `IORING_REGISTER_BUFFERS` on the 2 MiB arena |

`NO_IOWAIT` is **not** an `IORING_SETUP_*` flag. It is `IORING_ENTER_NO_IOWAIT` (bit 7) plus kernel feature `IORING_FEAT_NO_IOWAIT`. Call `khr_uring_enable_no_iowait()`.

## Registered ring fd

On init, register with `offset = (uint32_t)-1` so the kernel picks a free slot. The allocated index is written back to `up.offset` and stored in `ring->enter_fd`. Slot 0 is typical in a fresh process; it is **not** guaranteed after other rings have been created in the same task.

On destroy, `IORING_UNREGISTER_RING_FDS` **must** run before `close`. Closing the ring fd does not free the task's registered-ring slot. Leaking slot 0 makes the next `offset = 0` registration fail, and a stale `enter_fd = 0` without `IORING_ENTER_REGISTERED_RING` is stdin and will hang.

## SQ / CQ mapping

`get_sqe` indexes SQEs with `tail & mask` (correct for `NO_SQARRAY`). If a ring is ever created without `NO_SQARRAY`, `get_sqe` also writes `sq_karray[idx] = idx`.

Tail store is `memory_order_release`. CQ tail load is `memory_order_acquire`. `khr_uring_cqe_seen` publishes the new CQ head with `memory_order_release`.

## DEFER_TASKRUN

Requests do not run until `io_uring_enter` with `IORING_ENTER_GETEVENTS`. `khr_uring_submit` sets `GETEVENTS` when `min_complete > 0` **or** when `IORING_SETUP_DEFER_TASKRUN` is set (min_complete may still be 0 so it does not sleep). `khr_uring_tick` enters if `IORING_SQ_TASKRUN` or `IORING_SQ_CQ_OVERFLOW` is set.

## Waiting for CQEs

`khr_uring_wait_cqe_timeout` parks in the kernel when `IORING_FEAT_EXT_ARG` is set:

```c
struct io_uring_getevents_arg arg = { .ts = (uint64_t)&ts };
khr_sys_io_uring_enter2(enter_fd, submitted, 1,
                        enter_flags | GETEVENTS | EXT_ARG,
                        &arg, sizeof(arg));
```

`argsz` **must** be `sizeof(struct io_uring_getevents_arg)` (24 bytes). Passing `_NSIG/8` (the sigmask size used by the non-EXT_ARG `enter` wrapper) makes the kernel reject the wait with `-EINVAL`. That mismatch is why an earlier revision fell back to `nanosleep(50 µs)`, which costs ~1.2% of a 240 Hz frame and deschedules the presentation core.

When the kernel timeout expires, `io_uring_enter2` returns `-1` with `errno == ETIME`. `khr_uring_wait_cqe_timeout` recognizes this immediate completion and returns `false` without falling through into a redundant userspace spin loop, avoiding doubled timeouts and 100% CPU burns. Fallback to `khr_cpu_pause()` occurs strictly when EXT_ARG is unsupported on older kernels (`-EINVAL`/`-ENOSYS`) or on signal interruption (`-EINTR`).

## MSG_RING

```c
sqe->opcode = IORING_OP_MSG_RING;
sqe->fd     = dst_ring_fd;          /* real fd of Ring A, not enter_fd */
sqe->addr   = IORING_MSG_DATA;
sqe->off    = payload;              /* becomes dest CQE user_data (64-bit BDA) */
sqe->len    = KHR_MSG_RES_BDA;      /* becomes dest CQE res */
sqe->flags  = IOSQE_CQE_SKIP_SUCCESS;
```

The destination CQE's `user_data` **is** the payload. There is no separate tag on the dest CQE; we put `KHR_MSG_RES_BDA` (`0xBDA0`) in `res` so Ring A can recognize it.

`IOSQE_CQE_SKIP_SUCCESS` on the source means a successful MSG_RING posts **no** source CQE. Waiting with `min_complete = 1` on Ring B would block forever. Always link a NOP (`IOSQE_IO_LINK`) and wait for that NOP.

## MSG_RING as a cross-core doorbell

The same opcode wakes the compute worker: Core 2 submits `IORING_OP_MSG_RING` on Ring A (which it owns under `SINGLE_ISSUER`) with `fd = ring_b.ring_fd`, `off = 0`, `len = KHR_MSG_RES_WAKE` (`0x574B`). The worker parks in `khr_uring_wait_cqe_timeout` on Ring B; the doorbell CQE (`res = KHR_MSG_RES_WAKE`) drops it back into its drain loop, where it harvests completions and pops the SPSC cmdq. `ring_b.ring_fd` is stable for the worker's lifetime (published before `worker_ready`, torn down after `join`), so the producer side needs no lock. The 50 ms park backstop covers a lost doorbell (SQE exhaustion); `khr_topology_wake_worker()` is therefore best-effort by design.

## SQE helpers

All prep helpers call `khr_uring_get_sqe` (which zero-fills the SQE) and return `nullptr` if the SQ is full.

| Helper | Opcode |
|---|---|
| `khr_uring_prep_nop` | `IORING_OP_NOP` |
| `khr_uring_prep_msg_ring` | `IORING_OP_MSG_RING` |
| `khr_uring_prep_socket` | `IORING_OP_SOCKET` (supports direct descriptors via `file_index`) |
| `khr_uring_prep_connect` | `IORING_OP_CONNECT` (supports direct descriptors via `IOSQE_FIXED_FILE`) |
| `khr_uring_prep_accept` | `IORING_OP_ACCEPT` (supports direct descriptors via `file_index`) |
| `khr_uring_prep_close` | `IORING_OP_CLOSE` (supports direct descriptor slot release via `file_index`) |
| `khr_uring_prep_openat2` | `IORING_OP_OPENAT2` (supports direct descriptor slot installation via `file_index`) |
| `khr_uring_prep_read` | `IORING_OP_READ` (supports direct descriptors via `IOSQE_FIXED_FILE`) |
| `khr_uring_prep_write` | `IORING_OP_WRITE` (supports direct descriptors via `IOSQE_FIXED_FILE`) |
| `khr_uring_prep_read_fixed` | `IORING_OP_READ_FIXED` (supports direct descriptors via `IOSQE_FIXED_FILE`) |
| `khr_uring_prep_send` | `IORING_OP_SEND` (supports direct descriptors via `IOSQE_FIXED_FILE`) |
| `khr_uring_prep_recv` | `IORING_OP_RECV` (supports direct descriptors via `IOSQE_FIXED_FILE`) |
| `khr_uring_prep_recvmsg` | `IORING_OP_RECVMSG` (± `IORING_RECV_MULTISHOT`, `IOSQE_BUFFER_SELECT`) |
| `khr_uring_prep_sendmsg` | `IORING_OP_SENDMSG` (± `IOSQE_CQE_SKIP_SUCCESS`) |
| `khr_uring_prep_eventfd_watch` | `IORING_OP_POLL_ADD` with `KHR_POLLIN` (do **not** include `<poll.h>`) |
| `khr_uring_prep_fixed_fd_install` | `IORING_OP_FIXED_FD_INSTALL` + `IOSQE_FIXED_FILE` |
| `khr_uring_prep_timeout` | `IORING_OP_TIMEOUT` |

`khr_uring_prep_recvmsg(..., bgid, ...)`: pass `KHR_PBUF_NO_SELECT` (`0xFFFF`) to disable buffer select. `bgid == 0` is a real group (tier 0).

## Direct Descriptors (Registered Files Table)

Khoros completely bypasses the traditional process POSIX file descriptor table (`/dev/fd/*`) for high-throughput and real-time operations by using io_uring direct descriptors:

- **Sparse Table Registration**: `khr_uring_register_files_sparse(ring, nr)` invokes `IORING_REGISTER_FILES2` with `struct io_uring_rsrc_register { .flags = IORING_RSRC_REGISTER_SPARSE, .nr = nr }` (falling back to `IORING_REGISTER_FILES` with an array of `-1`s on older kernels). Ring A and Ring B initialize with 16 sparse slots by default.
- **Table Updates**: `khr_uring_register_files_update(ring, offset, fds, nr)` executes `IORING_REGISTER_FILES_UPDATE` to install active file descriptors directly into sparse slots without unregistering the table, or clears slots when passed `-1`.
- **Dedicated Slot Allocations**:
  - `KHR_DIRECT_SLOT_INGEST` (slot 0): Storage ingest pipeline.
  - `KHR_DIRECT_SLOT_WAYLAND` (slot 1): Wayland UNIX domain connection socket.
- **Direct Socket Creation**: `khr_uring_prep_socket` specifies `sqe->file_index = direct_slot + 1U`. On Linux 7.2, `SOCK_CLOEXEC` is explicitly stripped (`type & ~SOCK_CLOEXEC`) because direct descriptors are ring-private and never touch the kernel file table; passing `SOCK_CLOEXEC` to direct sockets returns `-EINVAL`.
- **Direct Connect & I/O**: `khr_uring_prep_connect`, `khr_uring_prep_send`, `khr_uring_prep_recv`, `khr_uring_prep_read`, `khr_uring_prep_write`, `khr_uring_prep_read_fixed` set `sqe->flags |= IOSQE_FIXED_FILE`, referencing the direct slot index directly without allocating a POSIX file descriptor or locking the process fd table.
- **Direct Close**: `khr_uring_prep_close` with `is_direct = true` sets `sqe->fd = 0` and `sqe->file_index = direct_slot + 1U`, releasing the registered slot directly inside the kernel without traditional `close(2)` context switches.

## Clock registration

```c
struct io_uring_clock_register clk = { .clockid = CLOCK_MONOTONIC };
khr_sys_io_uring_register(ring_fd, IORING_REGISTER_CLOCK, &clk, 0);
```

On Linux 7.2 the kernel rejects a **non-zero** `nr_args` (`if (!arg || nr_args) return -EINVAL`). Pass `0`. Default clock is already `CLOCK_MONOTONIC`; registration still matters for later absolute timeouts.

## User-data tags

SQE `user_data` values are four-character codes (`KHR_TAG_*` in `ring.h`) so harvested CQEs are identifiable. Dest MSG_RING CQEs are the exception: `user_data` is the 64-bit payload.

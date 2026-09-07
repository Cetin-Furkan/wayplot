# Kernel 7.2 Notes (read before touching uring)

Facts learned on `7.2.0-rc7-1-cachyos-rc`. Several of these differ from older man pages and from liburing defaults.

## `IORING_REGISTER_CLOCK`

Kernel:

```c
case IORING_REGISTER_CLOCK:
    ret = -EINVAL;
    if (!arg || nr_args)
        break;
    ret = io_register_clock(ctx, arg);
```

`nr_args` **must be 0**. Passing `1` (the usual “one object” convention) returns `EINVAL`. `arg` must be a zeroed `struct io_uring_clock_register` with `clockid = CLOCK_MONOTONIC` or `CLOCK_BOOTTIME`.

## Registered ring fds

- Register with `offset = (uint32_t)-1`. Kernel writes the slot back to `up.offset`.
- Forcing `offset = 0` fails once slot 0 is occupied.
- **Close does not unregister.** Always `IORING_UNREGISTER_RING_FDS` in `khr_uring_destroy`.
- `enter_fd` is a small index, often 0, not always 0. Tests must check `enter_flags & IORING_ENTER_REGISTERED_RING`, not `enter_fd == 0`.
- `io_uring_enter(0)` **without** `IORING_ENTER_REGISTERED_RING` is stdin. It hangs.

## `NO_IOWAIT`

Not an `IORING_SETUP_*` flag on this kernel. Feature bit `IORING_FEAT_NO_IOWAIT` (1u << 17), enter flag `IORING_ENTER_NO_IOWAIT` (1u << 7). This host advertises the feature (`features = 0x3ffff`).

## `DEFER_TASKRUN`

SQEs do not execute until enter with `IORING_ENTER_GETEVENTS`. `min_complete = 0` plus `GETEVENTS` flushes work without sleeping. `min_complete = 1` sleeps until a CQE exists.

## `IORING_ENTER_EXT_ARG` argsz

`khr_sys_io_uring_enter` always passes `argsz = _NSIG / 8` (sigmask). That is wrong for `IORING_ENTER_EXT_ARG`. Use `khr_sys_io_uring_enter2(..., &arg, sizeof(struct io_uring_getevents_arg))`.

With the correct size, `min_complete = 1` plus a `__kernel_timespec` is a real kernel timed wait. Do not `nanosleep` on Core 0. Fallback is `khr_cpu_pause()` (`PAUSE`).

## `IOSQE_CQE_SKIP_SUCCESS`

A successful request posts no CQE. `submit(..., min_complete = 1)` waits forever. Link a NOP (`IOSQE_IO_LINK`) and wait for the NOP. Used on MSG_RING (source side) and SENDMSG.

## MSG_RING payload

Destination CQE:

- `user_data` ← `sqe->off` (full 64-bit BDA)
- `res` ← `sqe->len` (`KHR_MSG_RES_BDA = 0xBDA0`)

Use Ring A's **real** `ring_fd` as `sqe->fd`, not `enter_fd`.

## `SINGLE_ISSUER`

Create Ring B on the worker thread. Creating it on main and submitting from the worker fails.

## PBUF

- `ring_entries` power of two
- `tail` is `uint16` overlaid on `bufs[0].resv`; only write `addr` / `len` / `bid`
- `recvmsg_out` name/control slots follow the **request** msghdr sizes

## `poll(` lint

`make lint` greps `poll(`. `IORING_OP_POLL_ADD` is fine. Function names must not end in `poll(`. Event bits: define `KHR_POLLIN = 0x0001` instead of including `<poll.h>`.

## Hugepages

`HugePages_Total = 0` on this host. `MAP_HUGETLB | MAP_HUGE_2MB` returns `ENOMEM`. Fallback: 2 MiB anonymous `mmap` + `MADV_HUGEPAGE` + **`MADV_COLLAPSE`** (25) so `khugepaged` is not left to collapse 4 KiB pages in the background. Skip `MAP_HUGETLB` under ASan. `MADV_COLLAPSE` may still succeed under ASan on the anon mapping.

## `O_DIRECT`

May fail on tmpfs (`/tmp`). Ingest retries without `O_DIRECT`. Keep test files 4 KiB-aligned.

## Digit separators vs 64 KiB

`64'536` == 64536. 64 KiB == `65'536`. PBUF tier 1 uses `65'536`.

## `io_uring_recvmsg_out`

16 bytes: `namelen`, `controllen`, `payloadlen`, `flags`. Then name slot, control slot, payload. `cqe->res` is payload bytes (recvmsg return).

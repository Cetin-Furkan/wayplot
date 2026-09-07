# Dual-Thread Topology

The topology object owns both rings, both PBUF tiers, the hugepage arena, and the compute worker.

## Files

- `include/khoros/core/topology.h`
- `src/core/topology.c`

## Constants

| Name | Value |
|---|---|
| `KHR_CPU_PRESENT` | 2 |
| `KHR_CPU_COMPUTE` | 3 |
| `KHR_HUGEPAGE_SZ` | 2'097'152 (2 MiB) |
| `KHR_STD_PAGE_SZ` | 4'096 |

If `sysconf(_SC_NPROCESSORS_ONLN) < 2`, both roles pin to the present CPU.

Cores 0/1 (SMT siblings 4/5 on the i7-1165G7) stay free for the kernel scheduler, IRQs, and the compositor. The engine never contends with the OS hot path: presentation owns physical core 2, compute owns physical core 3.

## Init sequence (`khr_topology_init`)

1. Zero the struct. No mutex, no condvar.
2. Pin the calling thread to core 2. Record `sched_getcpu()` as `present_cpu_actual`.
3. `mmap` the 4 KiB standard page (hot-loop scratch: msghdr, iov, timespec).
4. Allocate 2 MiB: try `MAP_HUGETLB | MAP_HUGE_2MB` (skipped under ASan). On failure, anonymous `mmap` + `MADV_HUGEPAGE` + `MADV_COLLAPSE`. `hugepage_collapsed` is set if either path yields a single 2 MiB TLB entry.
5. Init Ring A with `khr_uring_config_ring_a()`.
6. `khr_uring_register_clock(CLOCK_MONOTONIC)`, `khr_uring_enable_no_iowait()`, `IORING_REGISTER_IOWQ_AFF` to core 2.
7. Init PBUF tier 0 and tier 1 on Ring A.
8. `pthread_create` the worker. The worker pins to core 3, inits Ring B (**must** happen on that thread because of `SINGLE_ISSUER`), registers the hugepage as buffer index 0, records `compute_cpu_actual`, then `release`-stores `worker_ready`.
9. Core 2 `PAUSE`-spins on `worker_ready` (2 s cap, init path only). If `worker_failed`, destroy and return false.

## Worker commands (lock-free SPSC + kernel-parked wait)

Core 2 **never** `futex`-waits on Core 3, and Core 3 **never** spins. An 8-slot SPSC queue (`cmdq_head` / `cmdq_tail`, 60-byte pads so they do not share a cache line) carries commands; `IORING_OP_MSG_RING` from Ring A into Ring B (`res = KHR_MSG_RES_WAKE`, `KHR_TAG_WAKE`) is the doorbell that drops the worker out of its kernel-parked `wait_cqe_timeout` (50 ms backstop, so even a lost doorbell is recovered without burning a core).

Each queue slot `khr_wcmd_item_t` contains an inlined fixed-size buffer `char path[KHR_PATH_MAX]` (`KHR_PATH_MAX = 256`). When enqueuing ingest requests, `khr_cmdq_push` deep-copies the string path into the slot, guaranteeing that caller stack allocations cannot result in stale pointer reads or memory corruption on Core 3.

`khr_topology_signal_bda` enqueues one slot and doorbells. That is the presentation-thread path: drop a 64-bit BDA and keep rendering.

| `khr_wcmd_t` | Action on Ring B |
|---|---|
| *(empty queue)* | Park in `khr_uring_wait_cqe_timeout` (doorbell, DMA CQE, or 50 ms backstop) |
| `KHR_WCMD_MSG_BDA` | `IORING_OP_MSG_RING` of `u64` into Ring A (`res = KHR_MSG_RES_BDA`) |
| `KHR_WCMD_INGEST` | Submit async 3-SQE chain, return to parked wait; on CLOSE, MSG_RING byte count (`res = KHR_MSG_RES_INGEST`) |

Stop is a separate `_Atomic bool`, not a queue command. Ingest chains stay in flight across worker iterations: the op state (`ingest_path`, seen mask) is worker-owned, and a second INGEST arriving mid-flight is unpopped back to the queue head (single-consumer tail step-back, race-free) and retried after the next harvest.

## Completion pump (no tag-waiting, no staging array)

Ring A completions are harvested by `khr_topology_pump`: one kernel wait, then a non-blocking drain of everything posted, classified by `res` into three O(1) FIFOs — `bda_q` (64), `ingest_q` (16), `evt_q` (256, matching `KHR_RING_A_CQ_ENTRIES`). Wayland multishot packets and eventfd readiness land in `evt_q`; MSG_RING BDA/INGEST results land in their payload queues. No waiter blocks on a tag while unrelated completions pile up, and nothing is ever swallowed. `evt_q` overflow recycles the PBUF buffer immediately; parked buffers are recycled on consume or at destroy.

Public wrappers:

- `khr_topology_pump(topo, timeout_ms)` — harvest + classify; returns CQEs harvested.
- `khr_topology_wake_worker(topo)` — best-effort MSG_RING doorbell into Ring B.
- `khr_topology_signal_bda(topo, bda)` — **non-blocking**. Core 2 enqueues and doorbells.
- `khr_topology_wait_bda(topo, &out, timeout_ms)` — **must run on the Ring A issuer**. Pops `bda_q`, pumping as needed.
- `khr_topology_ingest(topo, path, &n)` — copies `path` into `cmdq` slot, enqueues ingest, doorbells, then reaps `KHR_MSG_RES_INGEST` from `ingest_q`.
- `khr_topology_arm_eventfd(topo, efd)` — `IORING_OP_POLL_ADD` on Ring A (swapchain-starvation stand-in).
- `khr_cmdq_push(t, cmd, u64, path)` — enqueues a command item into the lock-free SPSC queue with deep path copy.
- `khr_cmdq_pop(t, out)` — dequeues a command item from the lock-free SPSC queue.
- `khr_topology_pop_cqe(topo, &out_evt)` — pops a parked `evt_q` event, else pumps once.
- `khr_topology_wait_cqe(topo, &out_evt, timeout_ms)` — pops a parked event, else pumps with a wait.
- `khr_topology_staged_count(topo)` — parked `evt_q` depth (compat name).
- `khr_topology_recycle_cqe_buffer(topo, &evt)` — recycles a provided buffer (`IORING_CQE_F_BUFFER`) back to tier 0 or tier 1.

Do not put `alignas(64)` on members of `khr_topology_t`. That forces the whole struct to 64-byte alignment and blows up stack objects under UBSan/ASan. Pads of 60 bytes between the two atomics are enough.

## Destroy

`release`-store `stop`, MSG_RING doorbell (so the parked worker exits promptly), `pthread_join`. Worker destroys Ring B on its own thread. Parked `evt_q` buffers are recycled, followed by PBUF tier 0/1 unregister/destroy, Ring A destroy, and `munmap` of hugepage and standard page.

Tests that call `khr_topology_init` **must** call `khr_topology_destroy` on every path, including assertion failures.

## Memory footprint (as implemented, not the marketing 9 KiB)

Thread 1 also holds PBUF data (32 KiB + 1 MiB) and the 4 KiB scratch page. The “9 KiB ring” figure in the diagram is SQEs (64 × 64 B = 4 KiB) plus CQEs (256 × 16 B = 4 KiB) plus a small control page — the uring mapping only.

# wayplot measurements (2026-08-28)

Numbers below are from **one machine**, one kernel, one run of `make test`.
They are a baseline for later laptops (AMD iGPU, Turnip, etc.), not a promise.

## Host

| Item | Value |
|---|---|
| Machine | HP laptop (PCI subsystem `103C:87FE`) |
| CPU | Intel Core i7-1165G7 (Tiger Lake), 4C/8T, 0.4–4.7 GHz |
| Caches | L1d 192 KiB, L1i 128 KiB, L2 5 MiB, L3 12 MiB |
| RAM | 8 GiB (7.4 GiB visible) |
| GPU | Intel Iris Xe (TGL GT2), PCI `8086:9A49`, **integrated** |
| KMS driver | `xe` (`/dev/dri/card0`, `renderD128`) |
| Vulkan | API 1.4.354, instance 1.4.357, **Intel Mesa ANV** |
| Mesa | 26.2.1-arch3.1 (`vulkan-intel`) |
| OS | Arch Linux, CachyOS kernel |
| Kernel | `7.2.0-rc7-1-cachyos-rc` `x86_64` `PREEMPT_DYNAMIC` (built 2026-08-11) |
| Session | Wayland, `WAYLAND_DISPLAY=wayland-0`, GNOME |
| Compiler | GCC 16.2.1, `-std=gnu23` |
| wayplot | `VERSION` 0.1.0 |

Target of the project: Mesa iGPU office laptops. This box **is** that target (Intel, not NVIDIA). NVIDIA layers exist on the system; we do not use them.

## What this code is (today)

A **single-thread io_uring Unix-stream Wayland client** that has opened the compositor and picked a GPU:

- Kernel 7.2 ring: `SINGLE_ISSUER | DEFER_TASKRUN | COOP_TASKRUN | SUBMIT_ALL | CQSIZE | CLAMP | NO_SQARRAY`
- Enter: `GETEVENTS | REGISTERED_RING | NO_IOWAIT`, wait capped with `EXT_ARG` timespec
- Recv: provided-buffer ring **64 × 8192 B**, multishot `RECVMSG`, `POLL_FIRST`, `BUFFER_SELECT`
- Send: `SENDMSG` on the ring, `IOSQE_FIXED_FILE`
- FDs: `SCM_RIGHTS` (format table today; DMA-BUF/syncobj next)
- Userspace: `wp_wl_conn` concatenates the SOCK_STREAM into Wayland messages (`obj`, `size<<16|opcode`)
- Live GNOME: 40 globals, xdg 1280×720 (compositor sent 0×0), format table 200, 1 tranche, `main_device` 57984
- GPU pick: Iris Xe ANV 1.4.354, render node 57984 matches `main_device` (primary is 57856)
- Device: `hostImageCopy` proved, `pushDescriptor` max 32, `maintenance5` used, 4 exportable formats, **8 disjoint modifiers skipped**
- Present: 8 frames 1280×720, driver modifier `0x0100000000000002`, per-image release timelines
- Cube: 8 frames, 36 indices, push descriptors (UBO + sampled 1×1 host-copy albedo), per-slot depth

Old client used 256 × 64 KiB recv buffers (16 MiB). That is wrong for 8–32 byte Wayland requests. Current pool is 512 KiB.

Idle `wp_wl_pump_wait(..., 20ms)` with no peer traffic returns `-ETIME` in **20.04 ms** (kernel timeout, not a spin).

## Correctness (`make test`)

### Ring (`bin/normal/test-uring`)

Kernel accepted the full 7.2 flag set (`no_sqarray=yes`). Probe: NOP, SENDMSG, RECVMSG, SENDMSG_ZC, SOCKET, CONNECT, TIMEOUT, POLL_ADD all present.

| Check | Result |
|---|---|
| NOP round-trip | ~3.0–4.0 µs |
| `IORING_OP_TIMEOUT` | `-ETIME` |
| `REGISTER_FILES` | pass |
| Provided buffers | pass |
| `IOU_PBUF_RING_INC` register | pass (not used for recvmsg) |
| Multishot RECVMSG + `F_MORE` | pass |
| `SCM_RIGHTS` pipe fd still works | pass |

### Wayland-shaped conn (`bin/normal/test-wayland-conn`)

| Check | Result |
|---|---|
| 12-byte message (sync-shaped) | object/opcode/size/body match |
| 65532-byte message (near 16-bit max) | reassembled from 8 KiB CQEs, last byte intact |

### Bench (`bin/normal/test-uring-bench`)

Same pbuf as production (64 × 8 KiB). One ring, socketpair, registered fds. Wall times include submit + recv reassembly of **payload bytes**, not “CQEs per second”.

| payload | count | msg/s | bandwidth | wall |
|---|---|---|---|---|
| 8 B | 100000 | 2.42e6 | 18.4 MiB/s | 41.4 ms |
| 16 B | 100000 | 2.55e6 | 38.9 MiB/s | 39.2 ms |
| 32 B | 100000 | 2.59e6 | 79.0 MiB/s | 38.6 ms |
| 128 B | 50000 | 2.57e6 | 314 MiB/s | 19.4 ms |
| 1024 B | 20000 | 2.03e6 | 1.98 GiB/s | 9.9 ms |
| 4096 B | 8000 | 1.36e6 | 5.32 GiB/s | 5.9 ms |
| 8000 B | 4000 | 1.00e6 | 7.64 GiB/s | 4.0 ms |
| 65532 B | 200 | 1.66e5 | 10.4 GiB/s | 1.2 ms |

**How to read this**

- Typical Wayland request is 8–32 B. This CPU can move **~2.5 million** of those per second on a loopback socketpair. A 60 Hz client using even 10k messages/s is using well under 1% of that.
- Small-message rate is **completion-bound** (about 2.5M CQEs/s), not memcpy-bound. 8 B and 32 B have almost the same msg/s.
- Large payloads become **bandwidth-bound** (~10 GiB/s on loopback). A 64 KiB Wayland event (rare) is cheap.
- 64 KiB × many in-flight sends vs 512 KiB recv pool **deadlocks** if you do not cap inflight. The bench uses at most 2 huge sends at once. That is a real limit, not a microbenchmark artifact.

Loopback socketpair is **faster** than a real compositor (context switches, the server process, SCM_RIGHTS to Hyprland/GNOME). Treat these as an **upper bound**.

## Not used (on purpose)

| Feature | Kernel has it here | Why unused |
|---|---|---|
| `SENDMSG_ZC` / ZCRX | yes | NIC DMA; Unix Wayland is not a NIC |
| `SQPOLL` | yes | extra kernel thread for one client fd |
| `IOU_PBUF_RING_INC` on recvmsg | register works | can split `io_uring_recvmsg_out` from payload |
| Vulkan WSI / GLFW / libwayland | n/a | project refusals |

## Files that implement this

```
src/uring/ring.c       ring setup / SQ / CQ / EXT_ARG enter
src/uring/pbuf.c       provided-buffer ring
src/uring/sock.c       sendmsg / multishot recvmsg / cmsg fds
src/wayland/conn.c     stream send + reassembly + map + pump_wait
src/wayland/feedback.c owned format table + tranche pairs
src/vulkan/gpu.c       instance 1.4 + DRM match
test/uring/ring_test.c
test/wayland/conn_test.c
test/uring/bench.c
test/vulkan/gpu_test.c
```

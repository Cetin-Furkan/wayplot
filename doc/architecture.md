# Architecture: Wayplot Hardware Topology

Khoros is a Linux 7.2+ engine written in pure ISO C23. The runtime shape is the Wayplot dual-ring topology: two pinned threads, two `io_uring` instances, MSG_RING IPC through the shared L3, and no `liburing` / `libwayland` / `epoll` / OpenGL.

## Process map

```
CPU 0 (physical core 0)                         CPU 1 (physical core 1)
THREAD 1 — presentation / Wayland               THREAD 2 — ingest / compute
  Ring A  64 SQ × 256 CQ                          Ring B  128 SQ × 256 CQ
  CLOCK_MONOTONIC, NO_IOWAIT                      REGISTER_BUFFERS
  PBUF tier 0 (128 × 256 B)                       2 MiB hugepage / THP arena
  PBUF tier 1 (16 × 64 KiB)                       OPENAT2 → READ_FIXED
  4 KiB standard scratch page
                 ▲                                         │
                 └──── IORING_OP_MSG_RING (64-bit BDA) ────┘
```

`engine_init()` (`src/engine.c`) brings this topology up, prints the pin/clock/buffer report, posts the hugepage address through MSG_RING, harvests it on Ring A, then tears everything down. That is the `make run` smoke probe.

## Source layout

| Path | Role |
|---|---|
| `include/engine.h`, `src/engine.c`, `main.c` | Public banner + topology smoke init |
| `include/khoros/core/`, `src/core/` | CPU topology, worker, hugepages |
| `include/khoros/uring/`, `src/uring/` | Raw syscalls, rings, PBUF, async ingest ops |
| `include/khoros/wayland/`, `src/wayland/` | Wire encoder + uring client + non-blocking PBUF consumer |
| `include/khoros/gfx/`, `src/gfx/` | Vulkan 1.4 device, BDA arena, Slang pipelines, real offscreen frame |
| `tests/` | Seven suites + pump/pin/frame tests (81 total) |
| `doc/` | This documentation set |

## What exists today

Implemented and covered by tests:

- Dual `io_uring` rings with kernel 7.2 flags, registered ring fds, monotonic clock, `NO_IOWAIT`
- Dual-thread pin (CPU 0 / CPU 1), 2 MiB arena, `REGISTER_BUFFERS`
- MSG_RING 64-bit payload from Ring B into Ring A CQ
- Tiered PBUF rings + `io_uring_recvmsg_out` parser
- Wayland wire encode/decode and live compositor roundtrip (no `libwayland`)
- Multishot `RECVMSG` inbound and `SENDMSG` + `IOSQE_CQE_SKIP_SUCCESS` outbound
- `OPENAT2` / `READ_FIXED` ingest into the registered arena
- Eventfd starvation watch via `IORING_OP_POLL_ADD` (no `<poll.h>`)
- `IORING_OP_FIXED_FD_INSTALL` for later SCM_RIGHTS export

## What does not exist yet

See [graphics.md](graphics.md). The topology diagram still names:

- Vulkan 1.4 buffer-device-address render pass, push descriptors, dynamic scissor
- Slang shaders compiled to SPIR-V
- `wp_linux_drm_syncobj_surface` acquire/release
- KMS/DRM direct scanout and DMA-BUF import
- Cold-path `.obj` / `.dem` → `.wpbin` parser

Those sit on top of this kernel topology. Do not pretend they are present.

## Thread and ring ownership

`IORING_SETUP_SINGLE_ISSUER` is set on both rings. The thread that creates a ring is the only thread that may submit to it.

- Ring A is created on thread 1 after pinning to CPU 0.
- Ring B is created **inside the worker** after pinning to CPU 1.
- Main may peek Ring A CQEs (including MSG_RING deliveries). Main must never submit on Ring B.
- The worker must never submit on Ring A.

Destroy order: signal worker stop → join (worker destroys Ring B) → unregister PBUF → destroy Ring A → unmap hugepage and standard page.

## Related

- [topology.md](topology.md) — init sequence and worker commands
- [io-uring.md](io-uring.md) — ring flags and SQE helpers
- [kernel-notes.md](kernel-notes.md) — 7.2 ABI traps

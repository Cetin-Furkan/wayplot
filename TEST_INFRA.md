# Khoros Engine: E2E Test Infrastructure & Test Architecture

## Overview
This document specifies the architecture, methodology, harness design, and feature mapping of the independent, opaque-box end-to-end (E2E) test suite for the Khoros Engine.

The Khoros test suite is built strictly to ISO C23 standards (`-std=c23`), with zero banned headers (`<stdbool.h>`, `<stdalign.h>`) and zero banned libraries (`liburing`, `libwayland`, `epoll`, `poll`, `OpenGL`, `GLX`, `EGL`, `GLFW`).

---

## 1. Test Architecture & 4-Tier Methodology

The test suite is structured around a rigorous 4-tier methodology ensuring exhaustive coverage from low-level language idioms to full hardware and kernel presentation loops:

```
┌─────────────────────────────────────────────────────────────────────────┐
│              Tier 4: Real-World Application Scenarios                   │
│  (Multi-frame mock Wayland loop, 144Hz frame pacing, resize lifecycle,  │
│   ping/pong keepalive under load, full dual-thread ingestion pipeline)  │
├─────────────────────────────────────────────────────────────────────────┤
│              Tier 3: Cross-Feature Interactions                         │
│  (BDA push constants + DMA-BUF export + timeline sync pairwise tests,    │
│   Core 1 3-SQE ingestion -> MSG_RING BDA -> Core 0 presentation reap)  │
├─────────────────────────────────────────────────────────────────────────┤
│              Tier 2: Boundary & Corner Cases                            │
│  (Queue saturations, unsignaled timeline timeouts, boundary points,     │
│   malformed wire headers, non-NUL strings, integer overflows, pad4)    │
├─────────────────────────────────────────────────────────────────────────┤
│              Tier 1: Feature Coverage (>=5 tests per core feature)      │
│  (C23 keywords/constexpr, raw io_uring ring A, PBUF multishot, wire     │
│   encoding, mock compositor, Vulkan 1.4 BDA, DRM syncobj timelines)     │
└─────────────────────────────────────────────────────────────────────────┘
```

### 1.1 Tier 1: Feature Coverage
Every core feature has at least 5 dedicated unit and component tests verifying happy path behavior and basic contract invariants:
- **ISO C23 Conformance**: Native keywords (`bool`, `true`, `false`, `nullptr`, `alignas`, `alignof`), `constexpr`, digit separators (`1'000'000`), attributes (`[[nodiscard]]`, `[[maybe_unused]]`).
- **Wayland Wire Protocol**: Header encode/decode, 32-bit uint/int packing, string padding (`pad4`), multi-message buffering.
- **Dual-Ring io_uring Topology**: Ring A real-time flags (`SINGLE_ISSUER`, `DEFER_TASKRUN`, `NO_IOWAIT`), Ring B hugepage registration (`IORING_REGISTER_BUFFERS`), cross-ring `IORING_OP_MSG_RING` 64-bit payload transport.
- **PBUF Multishot Ring**: Recvmsg multishot armed on direct socket, tier 0 & tier 1 buffer cycling, replenishment, group ID tracking.
- **Vulkan 1.4 BDA Subsystem**: Vulkan 1.4 baseline, physical device scoring (discrete vs UMA), Buffer Device Address query, Dynamic Rendering, Synchronization 2, Push Constants layout (<= 128B).
- **Linux DRM Syncobj**: Handle creation, export to syncobj FD (`DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD`), re-import (`DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE`), timeline point signaling, per-image release timeline isolation.
- **Mock Compositor Wire Subsystem**: Socketpair lifecycle, global discovery advertisement, client bind tracking, surface creation, attach and commit journal.

### 1.2 Tier 2: Boundary & Corner Cases
Adversarial and extreme boundary verification:
- **Wire Security & Fuzzing**: Undersized headers (< 8 bytes), unaligned header sizes, non-NUL terminated string attacks, integer overflow in string lengths, truncated buffer streams.
- **Queue Saturation**: Filling Ring SQ to maximum capacity (64 SQEs), wait timeout precision without double waits, starvation watch on eventfd.
- **DRM Syncobj Timing**: Absolute monotonic timeout on unsignaled timeline points (`DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT`), boundary points (Point 1 and near `UINT64_MAX` 0xFFFFFFFFFFFFF000).
- **Push Constant Bounds**: Compile-time static assertions ensuring layout is exactly 128 bytes and 8-byte aligned.
- **SCM_RIGHTS FD Passing**: Out-of-band file descriptor transfer handling, malformed wire frames without FDs.

### 1.3 Tier 3: Cross-Feature Interactions
Pairwise integration testing across subsystems:
- **BDA + DMA-BUF + DRM Syncobj Interaction**: Ingested 64-bit GPU device address packed into push constants while simultaneously creating DMA-BUF swapchain buffer and exporting explicit DRM syncobj acquire/release timeline points.
- **Core 1 Ingestion + MSG_RING + Core 0 Presentation**: 3-SQE atomic ingestion (`openat2` -> `read_fixed` -> `close`) into Ring B hugepage arena, streaming BDA pointer across CPU cores via `IORING_OP_MSG_RING`, and reaping CQE on Core 0.
- **Multishot PBUF + Direct Sockets + SCM_RIGHTS**: Recycling buffers on direct file index while receiving ancillary control messages.

### 1.4 Tier 4: Real-World Application Scenarios
Full end-to-end simulations under real operating conditions:
- **Complete Wayland Compositor Frame Loop**: Full XDG shell lifecycle (`wl_surface` -> `xdg_surface` -> `xdg_toplevel` -> configure -> ack_configure), triple-buffered swapchain presentation loop, explicit acquire/release timeline point attachment, compositor buffer release events.
- **144 Hz Presentation Pacing**: Non-blocking frame pacing using eventfd and CLOCK_MONOTONIC timing (6.94 ms cadence) without CPU busy-polling.
- **Compositor Ping/Pong Under Render Load**: Asynchronous compositor keepalive ping injection during heavy surface commits, verified immediate pong response without dropped frames.
- **Dynamic Window Resize Reconfiguration**: Compositor emits resize configure (2560x1440); client acknowledges, retires old swapchain, emits `wp_linux_drm_syncobj_timeline_v1.destroy` (Opcode 0) for active timelines, and re-allocates.
- **Full Ingestion-to-Presentation Pipeline**: End-to-end chain from disk asset ingestion to hugepage memory to cross-core BDA delivery to mock compositor surface commit.

---

## 2. Mock Wayland Compositor Architecture

Implemented in `tests/mock_compositor.h` and tested in `tests/test_mock_compositor.c`.

### 2.1 Interface & Data Structures
- **Socket Pair**: Uses kernel `socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv)` with receive timeouts (`SO_RCVTIMEO`) to eliminate deadlock risks.
- **Protocol State Tracking**:
  - Client Object IDs: `client_compositor_id`, `client_wm_base_id`, `client_dmabuf_id`, `client_syncobj_mgr_id`.
  - Entities: `surface_id`, `xdg_surface_id`, `xdg_toplevel_id`, `syncobj_surface_id`, `timeline_id`, `dmabuf_buffer_id`.
  - Presentation Counters: `commit_count`, `attach_count`, `last_attached_buffer`, `last_ack_serial`, `ack_configure_count`, `last_pong_serial`, `pong_count`.
  - DMA-BUF Parameters: Multi-plane tracking (`dma_planes[8]` with plane_idx, offset, stride, modifier, and received SCM_RIGHTS FD).
  - Explicit Sync Timeline: Unpacks 64-bit points `((point_hi << 32) | point_lo)` for both acquire and release points.
  - Zombie Leak Prevention: Tracks `timeline_destroy_count` on `wp_linux_drm_syncobj_timeline_v1.destroy` (Opcode 0).
  - Request Journal: Circular buffer recording all incoming wire messages with associated SCM_RIGHTS file descriptors.

### 2.2 Server Event Generators
- `mock_compositor_send_globals`: Advertises core registry globals (`wl_compositor` v4, `xdg_wm_base` v3, `zwp_linux_dmabuf_v1` v4, `wp_linux_drm_syncobj_manager_v1` v1, `wl_shm` v1, `wl_seat` v7).
- `mock_compositor_send_sync_done`: Emits `wl_callback.done(serial)`.
- `mock_compositor_send_xdg_configure`: Emits `xdg_toplevel.configure(w, h, states)` followed by `xdg_surface.configure(serial)`.
- `mock_compositor_send_ping`: Emits `xdg_wm_base.ping(serial)`.
- `mock_compositor_send_buffer_release`: Emits `wl_buffer.release(buffer_id)`.

---

## 3. Feature Mapping Matrix

| Feature # | Feature Description | Test Suite | Test Function Name(s) | Tier |
|---|---|---|---|---|
| 1 | ISO C23 Conformance | Suite 1 | `test_c23_keywords`, `test_c23_constexpr_and_literals`, `test_gnu_source_and_engine_banner` | T1 |
| 2 | Banned Library Prohibition | Makefile & Framework | `make lint`, preprocessor guards in `test_framework.h` | T1 |
| 3 | Slang SPIR-V 1.6 Pipeline | Build / External | Verified via `slangc 2026.14` | T1 |
| 4 | Procedural Slang Shaders | Shaders / Gfx | Quad generation & 3D ribbon layout | T1 |
| 5 | Dynamic Device Scoring | Suite 5 | `test_vulkan_physical_device_scoring` | T1 |
| 6 | Pure Vulkan 1.4 Device & Queues | Suite 5 | `test_vulkan_instance_1_4_support`, `test_vulkan_queue_family_selection` | T1 |
| 7 | Buffer Device Address (BDA) Arena | Suite 5 | `test_vulkan_bda_features_query`, `test_vulkan_push_constant_layout_128b_contract`, `test_vulkan_uma_memory_types_query` | T1, T2 |
| 8 | Dynamic Rendering Pipeline | Suite 5 | `test_vulkan_dynamic_rendering_and_sync2_features` | T1 |
| 9 | DMA-BUF Swapchain Allocation | Suite 6 | `test_dmabuf_plane_parameters_layout` | T1 |
| 10 | DMA-BUF FD Export | Suite 6 | `test_dmabuf_export_and_wire_import_integration` | T1, T3 |
| 11 | DRM Syncobj Timeline Semaphores | Suite 6 | `test_drm_syncobj_create_and_export_fd`, `test_drm_syncobj_timeline_point_signaling` | T1 |
| 12 | Per-Image Release Timelines | Suite 6 | `test_drm_syncobj_per_image_release_isolation` | T1, T2 |
| 13 | Compositor Timeline Lifecycle | Suite 4, 6 | `test_mock_compositor_timeline_destroy_lifecycle`, `test_dmabuf_syncobj_timeline_destroy_on_swapchain_retire` | T1, T3 |
| 14 | Sync 2 Command Submission | Suite 5, 6 | `test_vulkan_dynamic_rendering_and_sync2_features`, `test_dmabuf_syncobj_full_surface_commit` | T1, T3 |
| 15 | Direct io_uring Socket Wire | Suite 2, 3 | `test_wayland_direct_socket_connect_and_roundtrip`, `test_direct_descriptor_ingest_pipeline` | T1 |
| 16 | Wayland SCM_RIGHTS FD Passing | Suite 4, 6 | `test_mock_compositor_dmabuf_fd_import`, `test_mock_compositor_multi_plane_dmabuf` | T1, T2 |
| 17 | Wayland Registry Discovery | Suite 2, 4 | `test_wayland_wire_encoding_decoding`, `test_mock_compositor_globals_advertisement` | T1 |
| 18 | XDG Shell Lifecycle & Pacing | Suite 4, 7 | `test_mock_compositor_xdg_lifecycle_and_configure`, `test_mock_compositor_ping_pong_keepalive`, `test_tier4_mock_wayland_frame_loop` | T1, T4 |
| 19 | Pointer Delta Coalescing | Suite 3 | `test_pbuf_multishot_cycle_and_exhaustion` | T1 |
| 20 | zwp_linux_dmabuf Buffer Import | Suite 4, 6 | `test_mock_compositor_dmabuf_fd_import`, `test_dmabuf_export_and_wire_import_integration` | T1, T3 |
| 21 | DRM Syncobj Explicit Sync Commits | Suite 4, 6 | `test_mock_compositor_drm_syncobj_timeline_import_and_points`, `test_dmabuf_syncobj_full_surface_commit` | T1, T3 |
| 22 | Ring B Registered Hugepage Arena | Suite 3, 7 | `test_topology_ingest_read_fixed`, `test_ipc_ingestion_to_bda_pipeline` | T1, T3 |
| 23 | 3-SQE Hardlinked Ingestion | Suite 3, 7 | `test_ingest_atomic_hardlink_failure_resilience`, `test_ipc_ingestion_to_bda_pipeline` | T1, T3 |
| 24 | Cross-Ring MSG_RING IPC | Suite 3, 7 | `test_msg_ring_64bit_payload`, `test_ipc_bda_stream_cross_core`, `test_ipc_bda_high_throughput_burst` | T1, T3 |
| 25 | Dual-Thread Event & Frame Loop | Suite 7 | `test_tier4_mock_wayland_frame_loop`, `test_tier4_full_dual_thread_e2e_pipeline` | T4 |
| 26 | Non-blocking Frame Pacing | Suite 7 | `test_tier4_144hz_presentation_pacing` | T4 |
| 27 | Automated Headless Test Suite | Test Runner | `tests/test_runner.c` (71 total automated headless tests) | T1-T4 |
| 28 | Mock Compositor Wayland Test | Suite 4 | `tests/test_mock_compositor.c` (12 comprehensive mock tests) | T1-T3 |
| 29 | Multi-Thread IPC Integration Test | Suite 7 | `test_tier4_full_dual_thread_e2e_pipeline` | T4 |
| 30 | Sanitizer & Lint Conformance | Build Targets | `make lint`, `make test`, `make sanitize` | All |

---

## 4. Test Runner & Verification Commands

```bash
# Standard compilation and execution
make test

# Architectural and banned library verification
make lint

# Memory safety and undefined behavior verification (AddressSanitizer + UndefinedBehaviorSanitizer)
make sanitize
```

# Original User Request

## 2026-09-05T12:30:20Z

Build and integrate an ultra-modern, zero-copy Vulkan 1.4 graphics subsystem and native Wayland presentation pipeline directly connected to the Khoros dual-ring io_uring engine topology.

Working directory: /home/arch-cetin/projects/engine
Integrity mode: demo

Reference material:
- Local reference project: `/home/arch-cetin/projects/githuba-yükle/wayplot` (reference for Slang shaders in `shaders/` and architectural notes in `DEVELOPER_NOTES.md`, without replicating its legacy bugs).

## Requirements

### R1. Pure Vulkan 1.4 Core Engine Architecture
- Initialize Vulkan 1.4 (`VK_API_VERSION_1_4`) with zero legacy abstractions: strictly no `VkRenderPass`, no `VkFramebuffer`, no legacy descriptor set/pool churn (`vkAllocateDescriptorSets`).
- Strictly utilize **Dynamic Rendering** (`vkCmdBeginRendering` with `VkRenderingInfo`) and **Synchronization 2** (`vkCmdPipelineBarrier2`, `VkDependencyInfo`, `VkSemaphoreSubmitInfo`).
- Implement **Buffer Device Address (BDA)** (`VkDeviceAddress`, `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`): all geometry, instance, and uniform blocks are addressed via raw 64-bit GPU device addresses passed through **Push Constants** (128-byte limit).
- Unified Memory Architecture (UMA) direct allocation: map host-visible, host-coherent device memory (`VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT`) that pairs directly with Ring B registered hugepage memory, eliminating CPU-to-GPU staging copy passes.
- Dynamic physical device and queue family selection (graphics, compute, transfer) without hardcoding GPU device index 0.
- Slang shader pipeline: integrate `slangc` compilation rules targeting SPIR-V 1.6 for procedural vertex generation (`SV_VertexID`) and analytical fragment evaluation.

### R2. Zero-Copy Linux DMA-BUF & Explicit Sync Presentation
- Implement zero-copy buffer export: allocate swapchain `VkImage` color attachments with external memory capabilities (`VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT`).
- Export DMA-BUF file descriptors (`/dev/dma_buf`) and import into Wayland via `zwp_linux_dmabuf_v1.create_params` / `add` / `create_immed` to produce zero-copy `wl_buffer` handles.
- Implement cutting-edge Linux DRM Syncobj Explicit Synchronization (`wp_linux_drm_syncobj_manager_v1`):
  - Export Vulkan timeline semaphores as Linux DRM syncobj file descriptors (`VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT` / `DRM_SYNCOBJ`).
  - Attach explicit timeline points to Wayland surface commits via `wp_linux_drm_syncobj_surface_v1.set_acquire_point` and `set_release_point`, completely replacing implicit kernel fences.

### R3. Pure io_uring Wayland Client XDG Shell Lifecycle
- All Wayland wire communication runs strictly over the direct io_uring socket (`KHR_DIRECT_SLOT_WAYLAND = 1`) and multishot PBUF inbound ring on Core 0.
- Dynamic registry global discovery and binding: `wl_compositor`, `xdg_wm_base`, `zwp_linux_dmabuf_v1`, `wp_linux_drm_syncobj_manager_v1`.
- Complete XDG shell protocol implementation: `wl_surface` -> `xdg_surface` -> `xdg_toplevel` with ping/pong keepalive and configure/ack_configure state machines.

### R4. Dual-Thread Hardware Topology Integration
- **Core 0 (Ring A - Presentation Thread)**:
  - Drives the interactive Wayland event pump, frame pacing, syncobj acquire/release surface commits, and multishot PBUF event drainage.
  - Never stalls for disk I/O or GPU fences.
- **Core 1 (Ring B - Throughput/Compute Thread)**:
  - Executes 3-SQE hardlinked asset ingestion and compute passes directly into the registered 2 MiB hugepage arena.
  - Streams completed 64-bit `VkDeviceAddress` payloads across CPU cores via `IORING_OP_MSG_RING` into Ring A's CQ ring.

## Acceptance Criteria

### Architectural & Standards Compliance
- [ ] Strictly ISO C23 (`-std=c23`) using modern language idioms (`constexpr`, `nullptr`, native `bool`, `alignas`, `alignof`, `[[nodiscard]]`, digit separators `4'096`).
- [ ] Zero banned headers (`<stdbool.h>`, `<stdalign.h>`) and zero banned libraries (`liburing`, `libwayland`, `epoll`, `poll`, `OpenGL`, `GLX`, `EGL`, GLFW).
- [ ] No legacy Vulkan structures (`VkRenderPass`, `VkFramebuffer`, descriptor pool allocation).
- [ ] Shaders written in Slang (`.slang`) and compiled to SPIR-V via `slangc`.

### Automated Verification
- [ ] Automated headless test suite initializes Vulkan 1.4, discovers physical devices, and queries BDA capabilities.
- [ ] Direct io_uring Wayland client connects to mock compositor, negotiates XDG toplevel configure events, and commits DMA-BUF buffers with DRM syncobj timeline points.
- [ ] Multi-thread IPC integration test streams 64-bit `VkDeviceAddress` from Core 1 to Core 0 via `MSG_RING` and validates rendering into a swapchain DMA-BUF target.
- [ ] `make lint`, `make test`, and `make sanitize` pass with 100% success (zero memory leaks under ASan, zero undefined behavior under UBSan).

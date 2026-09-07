# Graphics (implemented: headless Vulkan 1.4 frame)

`include/khoros/gfx/` and `src/gfx/` implement device scoring, the BDA arena, Slang pipelines, and a real offscreen frame (`frame.h`/`frame.c`). The engine links `-lvulkan`. No WSI, no `libwayland`: presentation travels over the io_uring Wayland socket.

## What the frame proves (test_gfx_real_submit_and_readback)

1. Offscreen `B8G8R8A8_UNORM` image + host-visible readback buffer.
2. `HOST_WRITE -> VERTEX_SHADER/SHADER_READ` barrier: `HOST_COHERENT` flushes CPU writes to DRAM, it does **not** invalidate GPU L1/L2. Without this barrier BDA-streamed geometry tears.
3. Dynamic rendering with clear, dynamic viewport + scissor (both declared dynamic by the pipeline; skipping either is `VUID-vkCmdDraw-07831`).
4. BDA push constants via maintenance6 `vkCmdPushConstants2KHR` when the loader resolves it, classic `vkCmdPushConstants` otherwise.
5. `vkQueueSubmit2` under the device submit mutex (one aliased `VkQueue` on UMA silicon) with a timeline signal + host `vkWaitSemaphores`.
6. Readback via `vkCopyImageToMemoryEXT` when host-image-copy is enabled and resolvable, transfer-engine `vkCmdCopyImageToBuffer` otherwise. The test prints which path ran.
7. Center-pixel assertion: a fullscreen opaque red card must read back red. A recorded-but-never-submitted command buffer cannot pass.

## Feature ledger (Vulkan 1.4, verified against headers 1.4.357)

Used functionally: `bufferDeviceAddress`, `timelineSemaphore`, `dynamicRendering`, `synchronization2` (submit2 + barrier2), `maintenance5` (buffer usage-flags2 on the readback buffer), `maintenance6` (push-constants2 record), `hostImageCopy` (queue-free readback), scalar block layout passthrough. `pushDescriptor` is enabled at device creation and its property queried, but no dummy descriptor is bound: the BDA-push-constant design intentionally carries zero descriptor resources (ORIGINAL_REQUEST R1 forbids descriptor churn), and binding an unused set layout to tick a box would be exactly the facade this rewrite removes.

## Still cutover (Wayland presentation glue)

Signal a DRM syncobj timeline; `wp_linux_drm_syncobj_surface` (global already discovered by the Wayland client). Direct scanout via DMA-BUF. `IORING_OP_POLL_ADD` on the DRM eventfd only if the swapchain is starved (`khr_topology_arm_eventfd` is the rehearsal).

## Mandates (from AGENTS.md / DEVELOPER.md)

- Vulkan 1.4 only (`VK_API_VERSION_1_4`)
- Synchronization 2 only (`vkQueueSubmit2`, `vkCmdPipelineBarrier2`)
- No OpenGL, GLX, EGL
- No `libwayland` (the C23 wire client already exists)
- Shaders: Slang (`.slang`) → SPIR-V
- Presentation: `VK_KHR_display` / `VK_EXT_direct_mode_display` and/or DMA-BUF import to the compositor we already speak
- This host advertises `VK_KHR_display`, `VK_EXT_direct_mode_display`, `VK_KHR_wayland_surface`, `VK_EXT_headless_surface`, instance 1.4.357, `/dev/dri/card0` + `renderD128`

## Intended data path

1. Ring B `READ_FIXED` lands bytes in host-visible memory that is **also** `VkDeviceMemory` with `bufferDeviceAddress`.
2. `vkGetBufferDeviceAddress` yields a 64-bit GPU pointer.
3. Worker MSG_RINGs that pointer into Ring A CQ (`user_data` = BDA, `res` = `KHR_MSG_RES_BDA`). That helper already exists.
4. Thread 1 push-constant / push-descriptor bind of that BDA. No descriptor pool.
5. Dynamic rendering, dynamic scissor (top/front CAD views).
6. Signal a DRM syncobj timeline; `wp_linux_drm_syncobj_surface` (global already discovered by the Wayland client).
7. Direct scanout via DMA-BUF. `IORING_OP_POLL_ADD` on the DRM eventfd only if the swapchain is starved (`khr_topology_arm_eventfd` is the rehearsal).

## What not to do

- Do not introduce `libwayland`, WSI convenience wrappers, or `vkQueueSubmit` (v1).
- Do not allocate descriptor sets.
- Do not copy ingest bytes CPU-side into a second staging buffer if the registered iovec can be the Vulkan allocation.
- Do not block the Ring A hot loop on GPU fences; MSG_RING + optional eventfd watch only.
- Tests must skip or stub when no GPU is present so CI without a device still builds.

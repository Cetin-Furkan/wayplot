# Device + DMA-BUF present (the step before the cube)

This file is the full record of **that** step: Vulkan 1.4 device, two timeline *roles*, three DMA-BUF images, explicit drm-syncobj, frozen `present_begin` / `present_end`, first clear-color window.

Hardware numbers: `docs/MEASUREMENTS.md`. Architecture walls: `docs/CONTEXT.md`. Wire/io_uring path up through GPU pick: `docs/WAYLAND.md`.

Live proof on this box (GNOME, Iris Xe, kernel 7.2): `test-engine-present` submitted **8 frames** at 1280×720, modifier `0x0100000000000002`, no `wl_display.error`.

---

## 1. Why this step existed

GPU pick (`src/vulkan/gpu.c`) only created a `VkInstance` and matched `feedback.main_device` to a physical device. A window needs:

1. A `VkDevice` that **calls** 1.4, not one that enables bits as decoration.
2. Images the compositor can scan out: DRM FourCC + modifier + dma-buf fd, not WSI.
3. Explicit sync the compositor understands: `wp_linux_drm_syncobj_*`, not implicit fences and not `wl_buffer.release`.
4. A host loop that can wait when all three images are busy **without** `poll()`/`epoll()` as the event loop.

The old parent client had all of this in `vulkan.c` + `engine.c`, with one timeline for acquire *and* release. That is the bug this step was not allowed to copy.

---

## 2. Module walls (what may include what)

| Module | Owns | Must not see |
|---|---|---|
| `src/vulkan/device.c` | instance, device, queues, acquire timeline, push-descriptor *layout*, drm render fd | Wayland object ids |
| `src/vulkan/negotiate.c` | compositor format table ∩ GPU exportable modifiers | Wayland ids, io_uring |
| `src/vulkan/swapchain.c` | 3 `VkImage`s, dma-buf fds, **per-image** release timelines | Wayland ids |
| `src/wayland/session.c` | `create_params` / `add` / `create_immed`, `import_timeline`, `set_*_point`, `attach`, `damage_buffer`, `frame`, `commit` | `Vk*` |
| `src/helper/drmfd.c` | open `/dev/dri/renderD*`, `DRM_IOCTL_SYNCOBJ_*` | Vulkan, Wayland |
| `src/engine/present.c` | glue: pick GPU images ↔ wl_buffers ↔ commit | cameras, meshes, shaders |

`wayland/feedback.h` is the only Wayland header Vulkan is allowed to include: `dev_t`, FourCC, modifiers. No object ids.

---

## 3. `VkDevice` — 1.4 features that are actually called

`wp_device_open(dev_t compositor_dev)`:

1. `wp_gpu_enumerate` + `wp_gpu_pick` (steal the instance; do not destroy it).
2. Query `VkPhysicalDeviceVulkan1{2,3,4}Features` and `Vulkan14Properties`.
3. Refuse the device if any of these are missing: `timelineSemaphore`, `dynamicRendering`, `synchronization2`, `pushDescriptor` with `maxPushDescriptors >= 2`, `maintenance5`, exportable timeline as `OPAQUE_FD`.
4. Enable **only** what we call. `hostImageCopy` is enabled if the GPU has it (this Iris Xe does). `shaderDrawParameters` and `maintenance6` stay off.
5. Device extensions (still not core in 1.4): external memory/semaphore fd, dma-buf, drm format modifier, physical device drm.
6. Queues: graphics family; dedicated transfer family if one exists without `GRAPHICS`. 1.4 graphics queues already advertise `TRANSFER_BIT`. On this box both are family 0.
7. Resolve `vkGetMemoryFdKHR`, `vkGetSemaphoreFdKHR`, `vkGetImageDrmFormatModifierPropertiesEXT`.
8. Command pool with `RESET_COMMAND_BUFFER_BIT`.
9. Push-descriptor set layout + pipeline layout (see §3.3).
10. One **acquire** timeline semaphore, exported as opaque fd.
11. Open the DRM **render** node matching `dev_t` (for `SYNCOBJ_EVENTFD`, not for rendering).
12. If `hostImageCopy`: prove it (§3.2). Failure is fatal for the device.

On this box after open:

```
vk device  hostImageCopy 1  pushDescriptor 1 (max 32)  maintenance5 1
  gfx family 0  xfer family 0  acquire_fd 17  drm_fd 19  render 57984
```

### 3.1 maintenance5

For every image we will allocate (swapchain and the host-copy prove image):

```
VkDeviceImageMemoryRequirements { pCreateInfo = &imageCreateInfo }
vkGetDeviceImageMemoryRequirements(device, &q, &memreq)
```

Then `vkCreateImage`, then allocate `memreq.memoryRequirements.size`. We do **not** create a dummy image to query size. That was the 1.3 workaround 1.4 made unnecessary.

Swapchain images also use `vkGetImageSubresourceLayout2` (1.4) for plane pitch/offset after bind.

### 3.2 hostImageCopy prove (`wp_device_prove_host_copy`)

A 4×4 `R8G8B8A8_UNORM` **linear** image with `VK_IMAGE_USAGE_HOST_TRANSFER_BIT`:

1. `vkGetDeviceImageMemoryRequirements` (maintenance5 again).
2. Create, bind `HOST_VISIBLE` if the type bits allow, else any type in `memoryTypeBits`.
3. `vkTransitionImageLayout` UNDEFINED → GENERAL (host).
4. `vkCopyMemoryToImage` known pixels.
5. `vkCopyImageToMemory` back.
6. `memcmp` — mismatch is `-EIO`, device open fails.

This is the 1.4 streaming contract. Scanout images do **not** get `HOST_TRANSFER` (that would change the modifier). Host copy is the upload path for later atlas/DEM; the prove is not a toy, it is the gate.

### 3.3 pushDescriptor layout

Not a no-op function pointer. At device init:

```
VkDescriptorSetLayoutCreateInfo.flags = PUSH_DESCRIPTOR_BIT
binding 0: UNIFORM_BUFFER, VERTEX|FRAGMENT
binding 1: COMBINED_IMAGE_SAMPLER, FRAGMENT
vkCreatePipelineLayout(setLayoutCount = 1)
```

`vkCreateDescriptorSetLayout` with that flag **fails** if the feature is off. Cube (next) binds both slots with `vkCmdPushDescriptorSet`. There is no descriptor pool and there will not be one for v1.

### 3.4 Memory type helper

`wp_find_memory_type(props, typeBits, prefer)`: first type in `typeBits` that has `prefer` (usually `DEVICE_LOCAL`); if none, **any** bit in `typeBits`. Never invent a type. Swapchain alloc: try `DEVICE_LOCAL`, retry without if `vkAllocateMemory` fails. Never silently drop `plane_count` to 1.

---

## 4. Timeline roles (the thing that is easy to get wrong)

### 4.1 Why “two timelines” is not “two objects”

A drm timeline is a counter. **Signaling point N signals every point ≤ N.**

Old client:

```
acquire_pt = ++timeline
release_pt = ++timeline
```

same `VkSemaphore`. GPU signaling image 1’s acquire can mark image 0’s release as done. Looks fine on a fast Hyprland+Mesa box. Wrong.

`wp_linux_drm_syncobj_surface_v1` also says: do **not** share a release timeline across buffers. Compositor may signal release points **out of order** (it can skip reading a buffer). Shared release + out-of-order signal ⇒ we think image 0 is free while the compositor still has it.

Correct split:

| Object | Count | Who signals | Who waits |
|---|---|---|---|
| Acquire timeline | **1** shared | GPU (`vkQueueSubmit2` signal) | compositor (`set_acquire_point`) |
| Release timeline | **1 per swapchain image** (3) | compositor (`set_release_point`) | GPU (submit wait) and host (`vkGetSemaphoreCounterValue` / eventfd) |

Acquire may be shared: we submit in order, so acquire 2 implies acquire 1, which is true.

Release must not be shared. Each image has its own `VkSemaphore` + opaque fd + drm handle + Wayland `import_timeline` id. Image 0’s release 1 does not move image 1’s counter.

### 4.2 Export / import

```
VkSemaphoreTypeCreateInfo { TIMELINE, initialValue = 0 }
  → VkExportSemaphoreCreateInfo { OPAQUE_FD }
vkCreateSemaphore
vkGetSemaphoreFdKHR → fd
```

Wayland:

```
wp_linux_drm_syncobj_manager_v1.get_surface(new_id, wl_surface)     // opcode 1
wp_linux_drm_syncobj_manager_v1.import_timeline(new_id) + SCM_RIGHTS fd  // opcode 2
```

One `get_surface` per `wl_surface`. Four `import_timeline`s: 1 acquire + 3 release. The fd is dup’d by `sendmsg`; we keep our copy for `SYNCOBJ_FD_TO_HANDLE`.

Mesa `OPAQUE_FD` for a timeline semaphore **is** a drm syncobj fd. That is the whole import story. We query `vkGetPhysicalDeviceExternalSemaphoreProperties` for `EXPORTABLE` before creating the device.

### 4.3 Points at commit

For slot `i`, after GPU submit:

```
set_acquire_point(acquire_timeline, acquire_pt)   // compositor waits for GPU
set_release_point(release_timeline[i], release_pt) // compositor will signal this
attach(wl_buffer[i])
damage_buffer(0,0,w,h)
frame(new callback)
commit
```

`acquire_pt` is monotonic on the shared acquire semaphore (`++device.acquire_point`).
`release_pt` is monotonic **on that image’s** semaphore (`img->last_release + 1`).

First present of an image: `last_release == 0`, GPU submit has **no** wait. Later presents of the same image wait on the previous `last_release` of **that** image.

Protocol: acquire and release must both be set iff a non-null buffer is attached. We always set both on a real commit.

If acquire and release were the **same** timeline object, acquire must be strictly less than release. They are different objects, so that rule does not apply.

### 4.4 Waiting when three images are busy

`wp_swapchain_pick`: for each image, `last_release == 0` **or** `vkGetSemaphoreCounterValue(release_sem) >= last_release`. If none, `-EAGAIN`.

Then we do **not** `vkWaitSemaphores` on the CPU (that would freeze Wayland pings). We:

1. `eventfd(EFD_CLOEXEC | EFD_NONBLOCK)` per busy image (kept until CQE; do not close before poll completes).
2. `DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE` already done at image create (`flags = TIMELINE`, fallback flags 0).
3. `DRM_IOCTL_SYNCOBJ_EVENTFD { handle, point = last_release, fd = eventfd }`.
4. `IORING_OP_POLL_ADD` on that eventfd, `user_data = POLL_UD | slot`.
5. `present_poll` still `pump_wait`s the Wayland recv. Completions are either compositor messages or the eventfd.

Render node is found by walking `/dev/dri/renderD*` and `fstat` `st_rdev == gpu.render`.

This is kernel 6.6+ `SYNCOBJ_EVENTFD` on kernel 7.2. It is the wait CONTEXT asked for: three busy images must not sleep until a mouse event.

---

## 5. Format negotiation

`wp_vk_query_formats` walks a ranked FourCC list:

| Rank | DRM | VkFormat |
|---|---|---|
| 0 | ARGB8888 | `B8G8R8A8_UNORM` |
| 1 | XRGB8888 | `B8G8R8A8_UNORM` |
| 2 | ABGR8888 | `R8G8B8A8_UNORM` |
| 3 | XBGR8888 | `R8G8B8A8_UNORM` |

For each modifier from `vkGetPhysicalDeviceFormatProperties2` + `VkDrmFormatModifierPropertiesListEXT`:

- skip `DRM_FORMAT_MOD_INVALID`
- skip `drmFormatModifierPlaneCount > 1` (disjoint). **Count them** (`skipped_disjoint`). Do not silently `plane_count = 1`.
- `vkGetPhysicalDeviceImageFormatProperties2` with `EXTERNAL_MEMORY_DMA_BUF` + `COLOR_ATTACHMENT` + `DRM_FORMAT_MODIFIER` tiling. Require `EXPORTABLE`.

`wp_negotiate(feedback, vk_formats, render, primary)`:

- Skip a tranche whose `target_device` is not 0 / `main_device` / our render / our primary. That is the hybrid-GPU rule.
- Score: scanout flag +1000000, format rank, non-linear +100.
- For the winning (format, tranche), **intersect all modifiers** of that FourCC in the tranche with the GPU list (the modifier *list* we pass to `vkCreateImage`, not a single modifier).

This box: 4 exportable formats, **8 disjoint modifiers skipped**, negotiated ARGB8888, 3 modifiers (linear + two Intel tiled). Driver then picked `0x0100000000000002` (CCS-tagged Y-tiling). Linear was acceptable; the driver is allowed to choose.

---

## 6. Swapchain images (3)

`WP_SWAPCHAIN_IMAGES = 3`. Not a VkSwapchainKHR. We are not WSI.

Per image:

1. `VkImageDrmFormatModifierListCreateInfoEXT` with the **intersected** modifier list.
2. `VkExternalMemoryImageCreateInfo` `DMA_BUF`.
3. `VkImageCreateInfo` 2D, negotiated `VkFormat`, extent = window, `DRM_FORMAT_MODIFIER` tiling, `COLOR_ATTACHMENT` only (no `HOST_TRANSFER`).
4. **maintenance5** memory query, then `vkCreateImage`.
5. `vkGetImageDrmFormatModifierPropertiesEXT` — the modifier the driver actually selected. If `plane_count != 1`, fail (we skipped disjoint; the driver must not surprise us).
6. Dedicated alloc + `EXPORT_MEMORY` dma-buf. Prefer `DEVICE_LOCAL`, retry any type in `memoryTypeBits`.
7. `vkBindImageMemory2`.
8. For each plane: `vkGetImageSubresourceLayout2` with `MEMORY_PLANE_n`. Store stride/offset.
9. `vkGetMemoryFdKHR` → `dma_fd`. Keep this fd for the process lifetime; Wayland `add` gets a `dup`.
10. Color `VkImageView`.
11. **Per-image release timeline** + `drm_syncobj_fd_to_handle`.
12. One primary command buffer from the device pool.

If any slot fails, destroy the ones already created. Do not leave a half swapchain.

### 6.1 Retire on resize

`wp_swapchain_retire` memcpy’s the 3 images into a growable `retired[]` and zeros the active slots (fds set to -1). It does **not** destroy: the compositor may still hold them.

`wp_swapchain_free_retired` (called from `present_poll`): for each retired image, if `vkGetSemaphoreCounterValue(release_sem) >= last_release` (or never presented), destroy Vulkan objects and close fds. If still busy, keep it.

`present_begin` sees `session->size_dirty` from a later `xdg_toplevel.configure` with a new positive size: retire, `wp_swapchain_create` at new extent, re-export wl_buffers, `ack_configure`, then pick.

---

## 7. Wayland buffer objects

`struct wp_wl_export` is POD: dma_fd, plane count, offset/stride[4], modifier, FourCC, width, height. No `Vk*`.

`wp_session_create_wl_buffer`:

```
zwp_linux_dmabuf_v1.create_params          opcode 1
for each plane:
  dup(dma_fd)
  zwp_linux_buffer_params_v1.add           opcode 1, 28 bytes + SCM_RIGHTS
zwp_linux_buffer_params_v1.create_immed    opcode 3, new wl_buffer id
zwp_linux_buffer_params_v1.destroy         opcode 0
```

`create_immed` (v2+) does not wait for a `created` event. Failure is a protocol error on the display, which `wp_registry_handle` already prints and returns `-EPROTO`.

`wl_surface` requests we added to the proto blob: `attach` 1, `frame` 3, `commit` 6, `damage_buffer` 9. We use `damage_buffer` (buffer coordinates), not `damage` (surface coordinates). First window is scale 1.

`surface.frame` allocates a `wl_callback`. `present_begin` refuses to pick until `frame_done` (true before the first commit). That is vsync backpressure **in addition to** release points.

Opcodes live in the relative-pointer proto blob. Missing lookup is `-ENOENT`, never opcode 0.

`syncobj_mgr` bind is **required**. GNOME advertises `wp_linux_drm_syncobj_manager_v1` v1.

---

## 8. Frozen present API

```c
int  wp_present_open(p, session)     /* device, negotiate, 3 images, export, import timelines */
int  wp_present_poll(p, timeout_ns)  /* wayland pump + drain retired + reap eventfds */
bool wp_present_begin(p, &frame)     /* false = closed / no image / waiting frame / resize fail */
int  wp_present_end(p, &frame)       /* submit + set points + attach + commit */
void wp_present_close(p)
```

`wp_present_frame` after a successful begin:

```
cmd, image, view, extent, scale, slot
```

`scale` is 1 until we bind `wl_output`.

**What begin did at the end of the present step:** recorded a full CLEAR and `EndCommandBuffer`. The cube step changed that: begin now only starts the command buffer and the color layout barrier. The renderer records. `present_end` calls `vkEndCommandBuffer`, then submit+commit. See `docs/CUBE.md`.

`present_end` in this step: `vkQueueSubmit2` waits the image’s previous release (if any), signals the shared acquire; then Wayland points + attach + damage_buffer + frame + commit.

Clear color is on `p->clear[4]`. Not a shader.

---

## 9. Proto blob additions this step

Interfaces now in `wp_proto_core` (15 total): previous 11 plus

- `zwp_linux_buffer_params_v1` — destroy 0, add 1, create 2, create_immed 3
- `wl_buffer` — destroy 0
- `wp_linux_drm_syncobj_timeline_v1` — destroy 0
- `wp_linux_drm_syncobj_surface_v1` — set_acquire_point 1, set_release_point 2

`zwp_linux_dmabuf_v1` gained `create_params` (1) next to existing `get_surface_feedback` (3).
`wl_surface` gained attach / frame / damage_buffer next to commit.
`wp_linux_drm_syncobj_manager_v1` gained get_surface / import_timeline.

Object map kinds: `SYNCOBJ_SURFACE`, `SYNCOBJ_TIMELINE`, `BUFFER`, `DMABUF_PARAMS`.

Intern **all** strings before writing `wp_proto_msg` arrays. Interning during a fill reallocs the blob and invalidates `m`.

---

## 10. io_uring pieces added this step

- `wp_wl_poll_add(conn, fd, POLLIN, slot)` → `IORING_OP_POLL_ADD`, `user_data = 0x504F4C00 | slot`.
- `reap` sets `conn.poll_ready` bit on success; ignores `TIME_UD`; still processes every CQE before `cq_advance`.
- Eventfd stays open until that bit is seen, then `read` + `close`. Arming again while an efd is live is skipped (do not stack POLL_ADDs).

Recv path unchanged: 64×8 KiB pbufs, multishot `RECVMSG`, `EXT_ARG` timeouts.

---

## 11. Tests that locked this step

| Binary | What it proves |
|---|---|
| `test-wayland-proto` | new opcodes (create_params 1, add 1, create_immed 3, attach 1, damage_buffer 9, frame 3, syncobj get_surface 1, import_timeline 2, set_acquire 1, set_release 2) |
| `test-vulkan-device` | device open, push layout, maintenance5 flag, acquire fd, drm fd, **host copy prove** |
| `test-engine-present` | negotiate + 3 dma-bufs + 3 release timelines + 8 live commits, 1280×720 |

`make test` uses `set -o pipefail`. A crash in the middle is a failed Make, not a successful `tee`.

Registry tests heap-allocate `wp_wl_conn` (~13 KiB of send slots). Do not put that struct next to a 108-byte `path[]` under stack-protector.

---

## 12. What we refused to simplify

- One timeline for acquire+release.
- One release timeline for three buffers.
- `vkWaitSemaphores` on the CPU instead of eventfd+uring when images are busy.
- `HOST_TRANSFER` on scanout images.
- Dummy `vkCreateImage` to query memory size.
- Enabling `pushDescriptor` without creating a push set layout.
- Enabling `hostImageCopy` without a round-trip prove.
- Silent `plane_count = 1` on disjoint modifiers.
- `create` + wait for `created` event instead of `create_immed` (we use immed; failure is still a display error).
- `poll()` as the wait loop.
- Binding a GPU that does not match `main_device`.
- `exit(1)` inside library code (return `-EIO` / `-ENODEV` / `-EPROTO`; tests and `main` decide).

---

## 13. Files

```
include/vulkan/device.h          include/vulkan/negotiate.h
include/vulkan/swapchain.h       include/engine/present.h
include/helper/drmfd.h           include/wayland/session.h  (export/commit/sync)
src/vulkan/device.c              src/vulkan/negotiate.c
src/vulkan/swapchain.c           src/engine/present.c
src/helper/drmfd.c               src/wayland/session.c proto.c
test/vulkan/device_test.c        test/engine/present_test.c
```

---

## 14. What the next step is (cube)

`present_begin` currently records the clear and **ends** the command buffer. That was legal for a clear-only window. A cube is `vkCmdDrawIndexed` of an **uploaded** mesh with **push descriptors**, a depth image, and a perspective camera. Begin must leave `cmd` open; the renderer records; end submits.

Not next: text, cards, DEM, a fullscreen fragment-shader “cube.”

# Developer note: present slots now, our swapchain much later

**Status: not for version 1.0. Do not start this work.** Far later, after the dual-ring engine is a real app (content in the BDA arena, window that follows XDG, run until close). This file is a parking lot so we do not “accidentally” grow a WSI clone in the 1.0 tree.

This is **not** `DEVELOPER.md` (C23 / bans / build). This is **not** `doc/graphics.md` (what gfx does today). Read those for current law. Read this only when someone says “we should add a swapchain.”

Recorded 2026-09-07. Do not rewrite history in `grok.ai` / `muse.ai`; append there if the decision changes.

---

## What we refuse (forever, including a later swapchain)

Vulkan WSI on Wayland is libwayland. The create info is `struct wl_display*` + `struct wl_surface*` (`VK_KHR_wayland_surface` in `vulkan_wayland.h`). Mesa then dispatches that connection (poll / epoll / `wl_display_dispatch`). That is a second owner of the compositor socket.

Banned in this project, later versions included:

- `VkSwapchainKHR`, `vkAcquireNextImageKHR`, `vkQueuePresentKHR`
- `VK_KHR_wayland_surface`, `vkCreateWaylandSurfaceKHR`
- `libwayland`, `wayland-client.h`
- `epoll`, `poll`, any wait on the compositor fd that is not raw `io_uring` on Ring A

A “later swapchain” that links WSI is not our swapchain. It is giving the window away.

`VK_KHR_display` / KMS is a different product (no Wayland). Not this note.

---

## What version 1.0 already is

A swapchain in the abstract is a small ring of showable images: acquire a free buffer, GPU paints it, present it, wait until the display is done, reuse.

1.0 already has that, without the Vulkan name:

| Abstract step | 1.0 code |
|---|---|
| Image count | `KHR_DMABUF_PRESENT_SLOTS` = 2 |
| Acquire | `khr_dmabuf_present_next_free` + `wl_buffer.release` |
| GPU target | LINEAR DMA-BUF-exportable `VkImage` per slot (no WSI present layout) |
| Present | DRM syncobj acquire/release points + `attach` + `commit` on Ring A |
| Idle | Ring A `io_uring_enter`; Ring B parked until MSG_RING |

Call them **present slots**. Do not name a 1.0 type `khr_swapchain_*`. That word in this repo currently means WSI in people’s heads.

Display path (do not change for 1.0):

Ring A pumps Wayland → free slot → `vkQueueSubmit2` into that image (presentation thread, one `VkQueue` on this UMA) → counter-query bridge onto the DRM acquire timeline → commit. Compositor shows the DMA-BUF. Release recycles the slot.

Big bytes stay on Ring B (2 MiB hugepage). Ring A only gets an 8-byte `VkDeviceAddress`.

---

## What “reinvent a swapchain” means here — later only

When we do it, we invent **our** object on top of present slots + io_uring Wayland. We do not wrap Khronos WSI.

A later module might own:

- Slot count as a real choice (2 vs 3), still DMA-BUF images we export
- Recreate: XDG size change → retire timelines → destroy images → new images → new `wl_buffer`s → same surface (1.0 still ignores configure `w/h` and uses 320×240)
- Optional GPU wait on the compositor release point (pipelining beyond `wl_buffer.release`)
- Format/modifier from `zwp_linux_dmabuf_v1` feedback instead of LINEAR ARGB8888-by-probe
- Direct Vulkan→DRM timeline if a future stack can export `SYNC_FD` (Xe today cannot; keep the counter-query seam)

All of that still: Ring A owns the socket, no `wl_display*`, no `VkSurfaceKHR`.

Until then, a resize is “destroy `khr_dmabuf_present_t`, `init` at the new size.” That is allowed in 1.0 if we need it. It is not a swapchain invention.

---

## Version 1.0 rule

If a change needs a new present abstraction, a `VkSurface`, or a second Wayland connection: it is this file’s work, not 1.0. Put it back.

1.0 work is content and the existing loop: BDA from Ring B, draw into the current slot, follow XDG when we choose to, stay up until close. Same two images. Same io_uring socket.

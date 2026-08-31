# Context for a future session

This is not a design spec and not a transcript. It is what you cannot recover from the files after this conversation is gone. Written for the same agent continuing the work.

This file is the long why; `AGENTS.md` is the short must-follow list. Product shape: `docs/HOST.md`. Step contracts in `docs/` (`DOC.md`, `PLAN.md`, `DEM.md`, `VIEW.md`, …) are what landed. If this file and a step doc disagree on *status*, the step doc wins.

---

## Who this is for

The author is building a **Wayland + Vulkan host** so people with **Intel/AMD iGPUs** (office laptops, Mesa ANV/RADV) can run scientific 3D and instrument-style cards **without NVIDIA VMs or translation layers**. ARM on Linux (Turnip) is later. Mali/PanVK may not be Vulkan 1.4 yet; v1 is “Mesa 1.4 drivers,” not every SoC. RISC-V GPUs are not a design target. **Do not spend time making NVIDIA work, and do not spend time spiting NVIDIA.**

Android is **not Wayland**. If it ever happens, it is a second present backend behind `present_begin` / `present_end`, not ifdefs in the Wayland files.

NPU (Intel NPU, AMD XDNA) is **not a Vulkan queue**. Do not put it in device init. iGPU compute + graphics is the scientific path.

The author writes C23, raw syscalls, and wire protocols by hand. Strength: metal (io_uring, DMA-BUF, drm syncobj). Recurring failure: **ceremony before mass** — unused Vulkan 1.4 feature bits, two fullscreen Slang files standing in for an engine, then ~100 empty nested directories (`wayland/resize`, `engine/present/begin`, `vulkan/device/pick`). Call that out early. Collapse folders; do not add more leaves.

They want `main.c` **next to the Makefile**. Do not nest a directory named `wayplot` inside this repo. Includes are `#include "wayland/....h"` with `-Iinclude`.

---

## Where the old code is

This repo is a **new tree**. The previous client is the **parent** of this folder (`../`):

- `vulkan.c` / `vulkan.h` — instance, DRM device match, modifier-list images, one timeline (wrong), DMA-BUF export
- `wayland.c` / `wayland.h` — send helpers, bind, seat, output
- `dmabuf_negotiate.c` — feedback ∩ GPU formats (keep the algorithm)
- `engine.c` — io_uring recv, present, **and** the entire protocol dispatcher (god file; do not recreate)
- `renderer.c` / `render_pipeline.c` / `shaders/*.slang` — two fullscreen passes (3D SDF dish + fake UI). **Art may be ported later as a volume node and a plot widget. Not as architecture.**

**Keep as knowledge, not as file shape:** device after `main_device`; modifier-list `vkCreateImage`; feedback snapshot; provided-buffer `recvmsg` layout; retired swapchain drain.

**Do not re-type DMA-BUF / feedback / uring recvmsg / retire-list from memory.** Open the old files. Do not grow the old two-shader renderer in place.

---

## What we already decided (architecture)

The old program was a **presentation substrate** with two fragment shaders pretending to be the app. A second card, real text, a mesh, or a DEM cannot grow out of that. The split:

1. Present (wayland + uring + vulkan swapchain) puts pixels on a compositor. It does not know cameras or widgets.
2. GPU resources: buffers, images, staging or host image copy, pipelines, **push descriptors**.
3. Host: views (rect + camera), resources as handles, a draw list, an input hit stack.
4. App: scientific content (mesh, heightmap, cards, plots).

CPU owns layout and scene. GPU shades samples. Text is glyph quads + atlas, not painting letters in a fullscreen UI shader. First `vkCmdDrawIndexed` of an uploaded mesh kills “the world is a fragment shader.”

**First window:** clear color only. No slang. Freeze:

```c
present_poll(...) -> events
present_begin(...) -> cmd, image, view, extent, scale   /* false = no free image */
present_end(...)
```

Pointer events are events. They do not mean “drag the one panel” inside the protocol dispatcher.

**Bootstrap order after that wall:** cube (push descriptors + upload) → real text → two CPU-tessellated cards → files (mesh/image) → heightmap / DEM. Not a better petri dish.

Error policy: compositor/GPU/kernel unusable → fatal. Bad mesh/font/DEM → error, process lives.

---

## Vulkan 1.4 — what “use it” means

The old client asked for 1.4 and enabled `pushDescriptor`, `maintenance5`, `maintenance6`, `shaderDrawParameters` without calling any of it. The renderer was 1.3 (dynamic rendering, sync2, timeline, push constants). `SV_VertexID` does not need `shaderDrawParameters`.

Require 1.4 on the device **and actually call**:

- **Push descriptors** — per-draw binds; no descriptor-pool architecture for v1.
- **Host image copy *or* a dedicated extra transfer queue** — 1.4 streaming contract. iGPU often wants host copy (shared memory). Elevation maps, font atlas, plot textures.
- **Maintenance 5** — `vkGetDeviceImageMemoryRequirements` before creating dummy images.
- Graphics/compute queues advertising `TRANSFER_BIT` (1.4 guarantee).
- Limits: 8K 2D is guaranteed; relevant for DEM tiles.

Still extensions in 1.4 (must query, still required for this client): dma-buf, drm format modifier, external fd memory/semaphore, physical device drm.

**Two timelines, not one.** Old bug: one timeline, `acquire = ++pt; release = ++pt`. Drm timeline signal(N) implies all points ≤ N. GPU signaling image 1’s acquire can mark image 0’s release as done. Works on a fast Hyprland+Mesa box; wrong elsewhere. Two `VkSemaphore` + two fds + two `import_timeline`: GPU-only acquire, compositor-only release.

`main_device` vs tranche `target_device`: negotiation must not pick a scanout tranche on another DRM node than the VkDevice. Hybrid AMD still exists without NVIDIA.

Caps tables (few KB): every GPU (name, apiVersion, drm nodes, features, modifiers, semaphore export, hostImageCopy vs transfer queue); compositor globals after `registry_done`; owned copy of the format table (do not keep the mmap forever). Print them on pick failure.

Debug utils optional via env. No WSI.

---

## Wayland and io_uring (author refusals)

**No** libwayland, liburing, wayland-scanner, libxml, GLFW. **No** poll/epoll as the **design** of the wait loop.

“No XML” means: the process does not parse XML and the build does not depend on scanner. It does **not** mean replace the Wayland wire. Hyprland/KWin/Sway speak object-id + size + opcode + args + SCM_RIGHTS. A relative-pointer blob on that socket is not a compositor protocol.

Internal style they want: **self-relative pointers (`wp_rp`, offset from blob/arena base)** for protocol *description*, registry, GPU caps, later meshes/DEMs/fonts. One primitive. Do not write a generic “marshal any message from the table” on day one. Hand-written sends (copy old `wl_send_msg` call sites) with opcodes stored in the blob; dispatch incoming by object map + opcode.

io_uring is the only wait loop for the compositor fd:

- Keep: `SINGLE_ISSUER | DEFER_TASKRUN | SUBMIT_ALL | CQSIZE`, provided buffers, multishot `RECVMSG`. **Store `io_uring_params`.**
- Add: `IORING_OP_SENDMSG` for **all** sends including SCM_RIGHTS (msghdr lives until CQE; prove one fd send before three swapchain exports). Old code blocked in `sendmsg`+`poll(POLLOUT)` — that hybrid is what we refused.
- `IORING_REGISTER_FILES`, packed `user_data` `{op, generation}`, timeout or poll on the **release syncobj fd** so three busy images do not sleep until a mouse event.
- `PBUF_SIZE` must fit `io_uring_recvmsg_out` + cmsg + max 16-bit Wayland message (65536 was slightly too small). 256×64K recv buffers was the wrong “few KB.”
- `-ENOBUFS` / CQ overflow: replenish, log, continue. Not `exit(1)`.
- Not fashion: `SEND_ZC` for 32-byte messages, `SQPOLL`, two rings on day one, file loads on this ring.

Registry is a **phase**: wait `wl_display.sync` (`registry_done`) before creating a surface. Old bug: surface as soon as four ids were non-zero; decoration/seat could arrive late.

Scale is stored in the old code and unused. Buffer = logical × scale when you get there. First window may be 1× with a warning if scale ≠ 1. Cursor `set_cursor` after enter was missing in the old client (Hyprland often still shows a cursor; **GNOME hides it**). wayplot now `set_shape` on enter via `wp_cursor_shape_manager_v1`, using the enter serial.

Required compositor: `wl_compositor`, `xdg_wm_base`, `zwp_linux_dmabuf_v1` ≥ 4, `wp_linux_drm_syncobj_manager_v1`. SSD optional.

Blocking `read` as a bring-up scaffold was offered and **refused**. They want uring from the first registry dump.

---

## Init sequence we agreed (first pixel)

1. Relative-pointer helpers + arena (when you start coding modules).
2. Protocol blob for display/registry/compositor/surface/xdg/dmabuf/syncobj — transcribed from old opcodes, not parsed XML.
3. Object map + registry table.
4. io_uring ring, register files, pbuf, timeout; connect; get_registry + sync as SENDMSG; print globals. **Done.**
5. Surface + empty commit; feedback snapshot; wait configure **and** feedback.done before ack. **Done** (owned table + expanded pairs; 0×0 → 1280×720).
6. Enumerate GPUs into caps; pick DRM ∩ 1.4 ∩ same `dev_t` as `main_device`; print everything on failure. **Done.**
7. Device with **used** 1.4 features; two timeline **roles**. **Done.** Acquire is one shared GPU timeline. Release is **one timeline per swapchain image** (the protocol forbids sharing a release timeline across buffers: signal(N) implies all points ≤ N). `hostImageCopy` is proved with `vkCopyMemoryToImage` round-trip. `maintenance5` `vkGetDeviceImageMemoryRequirements` before `vkCreateImage`. Push-descriptor set layout created (`maxPushDescriptors` 32 here). DRM render node open for `DRM_IOCTL_SYNCOBJ_EVENTFD`.
8. Port DMA-BUF images (old `create_vulkan_swapchain_images` + export); clear color; retire on resize from old drain code. **Done.** Three modifier-list images, `damage_buffer`, `create_immed`, per-image release import, dynamic rendering CLEAR. Busy images wait via `IORING_OP_POLL_ADD` on a syncobj eventfd, not `poll()`.
9. Freeze present API. **Done** (`present_poll` / `present_begin` / `present_end`). `begin` leaves `cmd` open; the renderer records; `end` submits.
10. Cube: uploaded indexed mesh, push descriptors, depth, host-copy albedo. Split into **mesh + camera + lit pass** (not a spinning-cube object). Raster: world CCW, camera Y-flip, `FRONT_FACE_COUNTER_CLOCKWISE`, `CULL_BACK` (Vulkan `a = -shoelace`). Locked by `test-math3d`, `test-renderer-mesh`, `test-renderer-spv`, `test-renderer-raster`. **Done.**
11. Text: CPU glyph quads + R8 atlas (FreeType, ASCII coverage), overlay pass with blend and `LOAD` after 3D. Same front-face pairing. Pixel Y-down, no extra flip. Locked by `test-renderer-font` and `test-renderer-text`. **Done.**
12. Resize: `xdg_toplevel.configure` was missing from the proto blob (size never updated). Now: parse size+states, integer scale (`preferred_buffer_scale` / `wl_output.scale`), buffer = logical × scale, `set_buffer_scale`, GPU-idle retire of DMA-BUFs, ack every configure serial. Pointer serials stored; move/resize requests exist; `main` does not drag the cube. **Done.**
13. Pass folding: `wp_pass` owns depth + `BeginRendering`/`EndRendering`. Opaque (clear color+depth) then overlay (load color, no depth). `wp_lit_draw` / `wp_text_draw` only bind + `DrawIndexed`. Locked by `test-renderer-pass` plus raster/text/cube updated to the pass. **Done.** See `docs/PASSES.md`.
14. CPU draw list: `wp_draw_list` in `src/engine`. Immediate clear/push/record into the two passes. Lit and text keep per-draw UBO (and text VBO/IBO) write heads so N items in one scope do not clobber. Locked by `test-engine-draw` (CPU array + GPU: two `"H"`, change a rect, pixels follow). **Done.** See `docs/LIST.md`.
15. Card mesh factory: `wp_card_cpu` tessellates a pixel-space rect (TL-BL-BR-TR, same winding as text). Overlay pipeline, solid color, no depth. `WP_DRAW_CARD` on the list (cards then text). Locked by `test-renderer-card`. **Done.** See `docs/CARD.md`.
16. Two cards as data: `struct wp_rect` is the source of truth; the list copies it and tessellates at record. Locked by `test-renderer-card` (magenta + green, shrink `w`, abandoned strip empty). **This step.** See `docs/CARDS.md`.
17. Next is not a new renderer. Later: instancing if both cards share a mesh and you measured it; hit stack; file mesh; DEM. Not bindless, not DGC, not a fullscreen card shader.

The old parent client used `CULL_NONE` + `FRONT_FACE_COUNTER_CLOCKWISE` on the 3D pipeline (`../render_pipeline.c`). That is the bug we are not repeating.

Do not enable `shaderDrawParameters` unless you use draw-id. Do not bind 1.4 extras you will not call.

Memory: prefer `DEVICE_LOCAL`, fall back to any type in `memoryTypeBits` (iGPU). Never silently `plane_count = 1` on alloc failure. Skip disjoint modifiers on the first window; ARM AFBC may disappear from the list until you implement disjoint binds — show that in the caps dump.

---

## Product definition of v1 (honest)

Done when: `make` on Intel or AMD Mesa 1.4 produces a window; cube or mesh; real text; two cards; integer HiDPI; caps dump on failure; no fullscreen-shader world; README lists dmabuf v4 + drm-syncobj.

Not v1: Android, NPU, RISC-V, Mali without 1.4, HarfBuzz, glTF PBR, planet-scale clipmaps, ECS, bindless, VMA, second io_uring, ImGui.

Old UI bugs that were **symptoms** (do not “fix” by growing slang): `menu_rect` 320×460 vs shader 580×340; titlebar steal with SSD; camera orbit override in the 3D shader.

---

## This repo’s filesystem (after the folder collapse)

We created ~100 empty leaves (`wayland/window/resize`, `vulkan/device/pick`, …). That was the same premature-structure hole. **Collapsed to six modules** under `src/` and `include/`: `helper`, `uring`, `wayland`, `vulkan`, `engine`, `renderer`. `test/` matches. `docs/` and `shaders/` kept. Split a directory when a **file** is actually fat.

Makefile: default `make` is **quiet** (`BUILD=normal`, no `-DDEBUG`). `make debug` / `make asan` compile in `wp_debug()` logs. `make release` is `-O2` and, if `main.c` or `src/**/*.c` changed since `.version-stamp`, bumps `VERSION` minor (`0.1.0` → `0.2.0`). `make release update` bumps major (`0.2.0` → `1.0.0`) even with no src change. Hash ignores `test/`, `docs/`, `include/`. First `make release` (no stamp) records a baseline and does not bump. GitHub is **not** tagged from Make; CI workflows still exist for when they want them.

`VERSION` is the source of truth. `build/<cfg>/version.h` is generated. `wp_debug` is a macro in `include/helper/log.h` (compiled out without `DEBUG`).

### io_uring (kernel 7.2, step 1)

Author kernel: `7.2.0-rc7` CachyOS. Ring lives in `src/uring/ring.c`. Tests: `make test` → `bin/<cfg>/test-uring`, log `build/test-uring.log`.

Use on this host:

- Setup: `SINGLE_ISSUER | DEFER_TASKRUN | COOP_TASKRUN | SUBMIT_ALL | CQSIZE | CLAMP | NO_SQARRAY`. Fallback drops COOP, then drops NO_SQARRAY (that last one is the 7.x win vs the old client, which always wrote `sq_array`).
- Enter always uses `IORING_ENTER_GETEVENTS` (required with DEFER_TASKRUN).
- Socket path to prove: `IORING_OP_SENDMSG` + `IORING_OP_RECVMSG` on an `AF_UNIX` `SOCK_STREAM` socketpair, recv with `IORING_RECVSEND_POLL_FIRST`. No `poll()`.
- Probe and log opcodes. Do **not** enable: SQPOLL, SQ_REWIND, SENDMSG_ZC / ZCRX (NIC DMA, not a compositor Unix socket), CQE32/SQE128, IOPOLL.

Step 2 (done in code): provided-buffer ring + multishot `RECVMSG` (`BUFFER_SELECT` + `POLL_FIRST`) + `SENDMSG` with `SCM_RIGHTS`, `REGISTER_FILES` / `IOSQE_FIXED_FILE`, `REGISTER_RING_FDS` + `ENTER_REGISTERED_RING`, `ENTER_NO_IOWAIT`, `IORING_OP_TIMEOUT`. Incremental `IOU_PBUF_RING_INC` is probed only — **do not use INC with recvmsg** (it can split `io_uring_recvmsg_out` from the payload). ZCRX / `SENDMSG_ZC` still unused: they are NIC DMA, not a compositor Unix socket.

Ideal for Wayland: **64 × 8 KiB** provided buffers (not 256 × 64 KiB). Typical requests are 8–32 bytes. A 64 KiB message is several CQEs assembled in `wp_wl_conn`. `make test` runs ring correctness, conn (12-byte + 65532-byte), and a msg/s bench (`test/uring/bench.c`). Large floods must cap in-flight sends so the socket + pbuf pool cannot deadlock.

Registry (this machine, GNOME `wayland-0`, 2026-08-28): connect via `IORING_OP_SOCKET` + `CONNECT` (libc fallback if that fails), `get_registry` + `sync` opcodes from a **relative-pointer proto blob** (not XML). 40 globals. Required: `wl_compositor` v6, `xdg_wm_base` v7, `zwp_linux_dmabuf_v1` **v5**, `wp_linux_drm_syncobj_manager_v1` v1. GNOME does **not** advertise `zxdg_decoration_manager_v1` (SSD is compositor policy / gtk_shell).

Session (same day): bind compositor/xdg/dmabuf/syncobj, `create_surface`, `get_surface_feedback`, empty commit, wait configure **and** `feedback.done`. GNOME configure is **0×0** (client default) → we use 1280×720. Feedback snapshot is an **owned copy**: 16-byte format-table entries memcpy’d off the mmap, tranche `u16` indices expanded to `{format, modifier}` pairs. `main_device` 57984 is the **render** node (`xe` `renderD128`); primary is 57856. `struct wp_feedback` has no Wayland object ids — Vulkan includes that header, not `session.h`.

Object map and `next_id` live on `wp_wl_conn`. Pump waits with `IORING_ENTER_EXT_ARG` so a 5 s cap is a kernel timeout, not a userspace check after a blocking enter. `make test` uses `pipefail` (a smashed registry test used to look like PASS because `tee` succeeded).

GPU pick (no `VkDevice` yet): instance 1.4, enumerate, match DRM render/primary to `feedback.main_device`. This box: Iris Xe ANV 1.4.354, `pushDescriptor` / `hostImageCopy` / `maintenance5` advertised, pick `[0]`. Next is device + two timelines + DMA-BUF images.

`./bin/normal/wayplot` prints registry + feedback + GPU pick.

Parent `../` still has the old flat client and `wayplot_compiled`. Do not dump new sources there.

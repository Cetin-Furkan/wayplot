# Wayland + io_uring + GPU pick (low-level, everything so far)

This is what the process is doing **right now**, byte by byte. Hardware and msg/s: `docs/MEASUREMENTS.md`. Architecture decisions: `docs/CONTEXT.md`.

The binary `./bin/normal/wayplot` connects to the compositor, creates an xdg toplevel, waits for DMA-BUF feedback, creates a Vulkan 1.4 device on the matching DRM node, exports three DMA-BUF images with explicit drm-syncobj, and presents a clear color until the window is closed.

---

## 0. What this program is

A Wayland client is not “Vulkan talking to the compositor.” It is:

1. One `AF_UNIX` `SOCK_STREAM` to `$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY` (here `/run/user/1000/wayland-0`).
2. Messages on that stream: 8-byte header + payload, 32-bit words, max size 65535.
3. File descriptors next to some messages (`SCM_RIGHTS`): format table, later DMA-BUF planes and drm syncobj.

Vulkan appears only **after** `zwp_linux_dmabuf_feedback_v1` has named a `dev_t` and a list of (FourCC, modifier) pairs. The GPU we create later must be that DRM node, not “the first Vulkan device.”

Typical request is 8–32 bytes (`display.sync` is 12). A fat `tranche_formats` array can approach the 16-bit limit. fds are rare but mandatory for the GPU path.

There is **no** libwayland, liburing, wayland-scanner, XML at runtime, poll/epoll loop, WSI, or GLFW. The kernel wait loop is `io_uring` on kernel **7.2.0-rc7**. The wire is still Wayland.

---

## 1. Kernel wait loop (`src/uring/ring.c`)

`io_uring_setup(64, flags)`. This machine accepted:

```
SINGLE_ISSUER | DEFER_TASKRUN | COOP_TASKRUN | SUBMIT_ALL | CQSIZE | CLAMP | NO_SQARRAY
```

| Flag / enter bit | What it does here |
|---|---|
| `SINGLE_ISSUER` | Only this thread may submit. One client thread. |
| `DEFER_TASKRUN` | Completion work runs when we `enter`, not as an IPI into userspace. |
| `COOP_TASKRUN` | Do not kick the task if it is running. |
| `NO_SQARRAY` | SQE index **is** `tail & mask`. The old client wrote `sq_array[i] = i` every submit. |
| `CQSIZE` | CQ = 4× SQ (256). |
| Enter `GETEVENTS` | Required with `DEFER_TASKRUN` or completions sit unprocessed. |
| `REGISTER_RING_FDS` | `io_uring_enter(enter_fd=0, …, REGISTERED_RING \| NO_IOWAIT)`. Avoids passing the ring fd every enter. |
| `EXT_ARG` | `io_uring_enter` can take a `io_uring_getevents_arg` with a timespec. That is how a wait has a real deadline. |

SQ/CQ head and tail are `_Atomic unsigned` with acquire/release. One producer (us), one consumer (kernel) on each ring. Not a multi-thread queue.

`wp_uring_get_sqe` does not publish `sq_tail` until `wp_uring_submit`, so the kernel never sees a half-filled SQE.

`wp_uring_submit_timeout(min_complete, ns)` sets `IORING_ENTER_EXT_ARG` and a relative `__kernel_timespec`. If the kernel returns `-ETIME`, no completion arrived in time. That is the 5 s registry/configure cap — **without this, `min_complete=1` blocks in the kernel forever and a userspace `now_ns()` check never runs.** A 20 ms idle `pump_wait` returns in ~20.04 ms (`test-wayland-conn`).

If `EXT_ARG` is missing, conn arms `IORING_OP_TIMEOUT` as an SQE (`off=1`, relative timespec on `conn.wait_ts`). Same idea, extra CQE.

Not used, on purpose: `SQPOLL` (extra kernel thread for one fd), `SENDMSG_ZC` / ZCRX (NIC DMA), `IOU_PBUF_RING_INC` on recvmsg (can split `io_uring_recvmsg_out` from payload).

---

## 2. Recv path (compositor → us)

```
IORING_OP_RECVMSG
  flags    = BUFFER_SELECT
  ioprio   = MULTISHOT | POLL_FIRST
  msg_flags= CMSG_CLOEXEC
  buf_group= 1
```

**Provided buffers:** 64 × **8192** bytes (512 KiB), `IORING_REGISTER_PBUF_RING`. The kernel picks a free buffer when data arrives. We `wp_pbuf_recycle` or the ring starves (`-ENOBUFS`).

Why 8 KiB, not 64 KiB × 256:

- Almost every Wayland message fits in one 8 KiB buffer (16-byte `io_uring_recvmsg_out` + cmsg + payload).
- A 64 KiB event is **several CQEs**. `wp_wl_conn` concatenates them (`in[]`). SOCK_STREAM has no message boundaries.
- Old 16 MiB pool was RAM for nothing.

Layout **inside** a provided buffer (kernel, not us):

```
[ io_uring_recvmsg_out 16 B ]
[ name : posted.msg_namelen bytes, we post 0 ]
[ control : posted.msg_controllen, we post CMSG_SPACE(8 ints) ]
[ payload : out->payloadlen ]
```

Offsets use the **posted** `msghdr` sizes, not `out->namelen`. Actual fd bytes use `out->controllen`.

`-ENOBUFS` means the pbuf ring was empty. We clear `recv_armed` and re-arm on the next pump. If we ignored that, a fat format table could freeze the client.

**Send:** `IORING_OP_SENDMSG`, `MSG_NOSIGNAL`. Payload is copied into a send slot (256 B inline, heap if larger) so the caller’s buffer can die before the CQE. Slot stays busy until that send’s CQE. `SCM_RIGHTS` lives in the slot’s cmsg. Short send (`res >= 0` but `res != len`) is `-EIO` — a partial Unix send would desync the Wayland stream.

**Fixed files:** compositor fd is `IORING_REGISTER_FILES` index 0. SQEs use `IOSQE_FIXED_FILE` and `fd = 0`.

**Connect:** `IORING_OP_SOCKET` (AF_UNIX, STREAM|CLOEXEC) then `IORING_OP_CONNECT` to `sockaddr_un`. Each wait has a 2 s `EXT_ARG` timeout. If either CQE fails, libc `socket`+`connect` then `adopt`.

**Reap:** every ready CQE is processed, then one `cq_advance`. A failed send must not skip recv CQEs in the same batch (that leaked pbufs). Extra `SCM_RIGHTS` fds that do not fit `pending_fds[8]` are `close`d, not dropped.

**Incoming fds:** each recvmsg’s cmsg fds are appended to `pending_fds[]`. Parsing a message that has an fd argument calls `wp_wl_take_fd` (FIFO). Wayland associates fds with messages in arrival order. `format_table` is the first fd we consume.

**Assembly buffer** `in[]` grows by doubling, capped at `4 * 65536`. `peek` requires a complete header and `size` bytes. `consume` memmoves the rest to index 0. `msg.raw` is the full message including the 8-byte header; strings are indexed from word 0.

---

## 3. Wire format

Every message:

```
u32 object_id
u16 opcode | u16 size     // size includes the 8-byte header, multiple of 4
[ payload size-8 bytes ]
```

Strings: `u32 length` (includes NUL) + bytes + pad to 4.  
Arrays: `u32 length` + bytes + pad to 4.  
New ids: client allocates, server never reuses until `delete_id`.

`display` is object **1**. We allocate 2, 3, …  
`get_registry` is 12 bytes: `[1][(12<<16)|1][new_id]`.  
`sync` is 12 bytes: `[1][(12<<16)|0][callback_id]`.  
Opcodes come from the proto blob, not from memory. A missing proto lookup is `-ENOENT`, not a silent opcode 0.

`wp_wl_str_at` / `wp_wl_array_at` / `wp_wl_put_str` (`src/wayland/wire.c`) take the **full** message and a word index. They do not assume `body` is `raw+8`.

---

## 4. Protocol blob (not XML)

`wp_proto_core()` builds one malloc’d region. Strings and `wp_proto_iface` / `wp_proto_msg` arrays live in it. A `uint32_t` field is a **byte offset from `base`**, never a pointer. The blob can be dumped or mmap’d. The process never parses XML.

Today: `wl_display`, `wl_registry`, `wl_callback`, `wl_compositor`, `wl_surface` (commit only), `xdg_wm_base`, `xdg_surface`, `xdg_toplevel`, `zwp_linux_dmabuf_v1` (`get_surface_feedback`), `zwp_linux_dmabuf_feedback_v1` (7 events), `wp_linux_drm_syncobj_manager_v1`. Lookup: `wp_proto_request(p, "wl_display", "get_registry")` → opcode 1.

We do **not** marshal arbitrary messages from the table. Sends are hand-written with opcodes stored in the blob. Args (`nargs`) are unused. Add requests when a call site needs them (`attach`, `damage_buffer`, syncobj `import_timeline`, …).

---

## 5. Object map (per connection)

Wayland ids are one namespace per socket. The map lives on `wp_wl_conn`, next to `next_id`. Not on the registry.

`wp_map` is a dense array, grown by doubling, indexed by id. `kind` + `version`. `wp_wl_alloc_id` is `++next_id` (display is 1, first alloc is 2). We never reuse an id until `wl_display.delete_id`; that event calls `wp_map_del`. Dispatch of an unknown / deleted id is a no-op.

Putting the map on the registry was wrong: compositor, surface, feedback, xdg objects are not “registry state.” They are connection state. Vulkan never sees this map.

---

## 6. Registry (a phase, not the loop)

1. Path = `XDG_RUNTIME_DIR` + `/` + `WAYLAND_DISPLAY`.
2. Ring + pbuf + SOCKET + CONNECT + register fd + arm multishot recv.
3. Send `get_registry` (id 2) and `sync` (id 3).
4. `pump_wait` until `callback.done` on id 3, remaining-time cap 5 s.
5. Each complete message: `wp_registry_handle` — `global` (name, interface string, version), `error`, `delete_id`.

On this GNOME session that is **40 globals**, including `zwp_linux_dmabuf_v1` **v5** and `wp_linux_drm_syncobj_manager_v1` v1. No `zxdg_decoration_manager_v1` (Mutter SSD is not that protocol). Required: compositor, `xdg_wm_base`, dmabuf ≥ 4, drm syncobj.

The registry is finished when `sync` returns. Creating a surface before that was the old client’s “four ids non-zero” bug.

---

## 7. Session: bind, surface, feedback snapshot

`wp_session_setup_surface`:

1. `wl_registry.bind` compositor (v≤6), `xdg_wm_base` (v≤6), `zwp_linux_dmabuf_v1` (v≤4), syncobj manager (optional, id stored).
2. `wl_compositor.create_surface`.
3. `zwp_linux_dmabuf_v1.get_surface_feedback` (opcode 3) — **surface** feedback, not default feedback. Scanout-capable tranches are about this surface.
4. `xdg_wm_base.get_xdg_surface` + `get_toplevel` + `set_title`.
5. Empty `wl_surface.commit`.
6. Wait until **both** `xdg_surface.configure` (serial) **and** `feedback.done`. Ping is answered. `delete_id` / `error` still go through `wp_registry_handle`.
7. If toplevel size is 0×0 (GNOME “you pick”), use **1280×720**. A 0×0 Vulkan image is not a window.
8. `xdg_surface.ack_configure(serial)`.

Dispatch is `wp_session_dispatch`: registry handle first, then xdg ping/configure/close, then feedback. There are not two different interpretations of `wl_display.error`.

### Format table and tranches (the actual GPU input)

`zwp_linux_dmabuf_feedback_v1.format_table` arrives with an fd + byte size. Each entry is 16 bytes:

```
u32 format     /* DRM FourCC */
u32 pad
u64 modifier
```

We `mmap`, **memcpy into an owned buffer**, `munmap`, `close(fd)`. The mmap is not kept (the compositor can recycle the fd; we need the bytes after `done`).

`tranche_formats` is an array of `u16` **indices** into that table, not pairs. We expand each index to `{format, modifier}` on the current tranche. Counting `nbytes/2` and throwing away the indices would make negotiation impossible.

`main_device` and `tranche_target_device` are `dev_t` in a Wayland array. On this box `main_device = 57984` (`makedev` of the **render** node). The primary node is `57856`. Matching the wrong one is the hybrid-GPU footgun.

`struct wp_feedback` (`include/wayland/feedback.h`) has **no object ids and no uring**. Vulkan may include it. That is the wall: compositor offer is DRM fourcc + modifiers + `dev_t`. Vulkan will later query its own modifier list and intersect. It must not include `session.h`.

On this GNOME / Iris Xe run: 200 table entries, 1 tranche, 200 pairs, flags 0 (not scanout), devices equal.

---

## 8. GPU pick (`src/vulkan/gpu.c`) — one step after the snapshot was real

`vkCreateInstance` with `apiVersion = VK_API_VERSION_1_4`. Enumerate physical devices. For each, query:

- name, `apiVersion`, vendor/device id
- `VK_EXT_physical_device_drm` → render/primary `dev_t`
- device extensions: dma-buf, drm modifier, external memory fd, external semaphore fd
- `VkPhysicalDeviceVulkan14Features`: `pushDescriptor`, `hostImageCopy`, `maintenance5` (advertised, not enabled yet — we have no `VkDevice`)

Pick the first GPU that is 1.4 + dma-buf + modifier + both fd extensions **and** whose render or primary node equals `feedback.main_device`.

This machine:

```
[0] PICK Intel(R) Iris(R) Xe Graphics (TGL GT2)  api 1.4.354 vendor 0x8086 device 0x9a49
    drm yes render 57984 primary 57856  dma-buf 1 modifier 1 memfd 1 syncfd 1
    1.4 1 pushDescriptor 1 hostImageCopy 1 maintenance5 1
    main_device 57984  (matches render, not primary)
```

No `VkDevice`. No swapchain images. No timelines. That is the next step, not this one.

---

## 9. Design review (what would have hurt)

Looked at the code after bind/surface/feedback, **before** adding Vulkan. These were already starting:

**Hollow snapshot.** `format_table` was mmap’d, count taken, `munmap`’d. `tranche_formats` only incremented a counter. GPU pick would have had a `dev_t` and a number, no formats. Fixed: owned table + expanded pairs in `wp_feedback`.

**Feedback lived inside session.** `session.h` pulls `conn.h` pulls `io_uring.h`. Vulkan including session would glue object ids and the ring into the GPU module. Fixed: `wayland/feedback.h` is a POD snapshot.

**Map on the registry.** Ids are a connection namespace. Binding compositor into `reg.map` made “registry” a god object. Fixed: `conn.map`.

**5 s deadline was a lie.** `io_uring_enter(..., min_complete=1)` with no timespec blocks until a CQE. The `now_ns() > deadline` check only runs after enter returns. A hung compositor hung us. Fixed: `EXT_ARG` timeout (20 ms idle test).

**`reap` on send error advanced the whole CQ batch and returned.** Recv CQEs in that batch were not recycled → pbuf leak → later `-ENOBUFS` freeze. Fixed: process every CQE, remember first error, then advance.

**Silent opcode 0.** `op ? op->opcode : 0` on a missing proto entry would send `wl_surface` attach as opcode 0 (`destroy` on some ifaces). Fixed: missing lookup is `-ENOENT`.

**GNOME configure 0×0.** That means “default size.” Creating a 0×0 DMA-BUF image is the next crash. Fixed: 1280×720.

**Extra SCM_RIGHTS fds dropped without `close`.** Leak. Fixed.

**`make test | tee` swallowed failures.** Pipeline status was `tee`’s. Registry stack-smash (`path[108]` next to a 13 KiB `conn` on the stack, canary checked at `main` return) looked like PASS. Fixed: `set -o pipefail`; registry test heap-allocates `conn`.

**Two wait loops.** Registry and session both pumped. `display.error` after registry was a bare `opcode==0`. Fixed: `wp_session_dispatch` always calls `wp_registry_handle` first.

Not bugs, constraints we are keeping: one ring per conn; 8 KiB pbufs + assembly; no ZC/SQPOLL; hand-written sends; no generic marshaler; `delete_id` clears the map but we still do not recycle ids (we only ever `++next_id`).

Still a smell, not a wall: `wp_wl_conn` is ~13 KiB (32 send slots × 256 B inline). Fine on an 8 MiB stack as one object; do not nest several on the stack next to small ssp-protected arrays. Tests heap-allocate when they also have a `path[]`.

---

## 10. Device + present (this step)

`wp_device_open` creates a Vulkan **1.4** device after DRM pick. Features that are **enabled because they are called**:

- `maintenance5` — `vkGetDeviceImageMemoryRequirements` on the modifier-list `VkImageCreateInfo` **before** `vkCreateImage`.
- `hostImageCopy` — `vkTransitionImageLayout` + `vkCopyMemoryToImage` + `vkCopyImageToMemory` on a 4×4 linear image at device init (must round-trip).
- `pushDescriptor` — descriptor set layout with `VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT` (UBO + combined image sampler). Cube/text will `vkCmdPushDescriptorSet`; the layout is not decoration.
- `dynamicRendering` + `synchronization2` + `timelineSemaphore` — first pixel is a CLEAR via `vkCmdBeginRendering` / `vkQueueSubmit2`.

`shaderDrawParameters` and `maintenance6` are **not** enabled.

### Two timeline **roles**, not two objects

`wp_linux_drm_syncobj_surface_v1` says signaling point N on a timeline signals every point ≤ N, so **one release timeline for three buffers is a protocol-level bug**. We do:

- **1 acquire timeline** (GPU signals, compositor waits). GPU submit order is our order; sharing is safe.
- **1 release timeline per image** (3). Compositor may signal out of order.

When all three images are busy, `present_begin` returns false and arms `DRM_IOCTL_SYNCOBJ_EVENTFD` + `IORING_OP_POLL_ADD` on that eventfd. The wait loop is still the compositor io_uring (recv + poll), not `poll()`.

DMA-BUF path (from the old client, not from memory): modifier **list** create, driver picks, `vkGetImageSubresourceLayout2`, `vkGetMemoryFdKHR`, `create_params` + `add` (dup fd) + `create_immed`, `set_acquire_point` / `set_release_point`, `attach`, `damage_buffer`, `frame`, `commit`.

Negotiation skips disjoint modifiers (`plane_count > 1`, 8 skipped on this Iris Xe) and any tranche whose `target_device` is not this GPU. Prefer scanout, then ARGB8888, then non-linear. This box: ARGB8888, driver chose CC-tagged Y-tiling `0x0100000000000002`.

Frozen present API:

```
present_poll(timeout_ns)     /* wayland + uring + drain retired */
present_begin() -> false     /* no free image, waiting frame callback, or resize */
present_end()                /* submit two-role timelines, commit */
```

First window: clear (0.07, 0.16, 0.22). No slang. Resize retires the old three images until their release points fire.

Live test: **8 frames** presented at 1280×720.

## 11. What is not done

- Real text, two cards, integer HiDPI / `wl_output` scale
- Seat / pointer / cursor — **done** (`wp_cursor_shape_manager_v1` on enter; edge resize / top-band move)
- Host image copy of font atlas / DEM (the path is proved; not wired to present images)

Init sequence from `docs/CONTEXT.md`: steps 1–6 are in the tree. Step 7 is the device.

---

## Files

```
src/uring/ring.c      setup / SQ / CQ / EXT_ARG enter
src/uring/pbuf.c      provided-buffer ring
src/uring/sock.c      sendmsg / multishot recvmsg / cmsg
src/wayland/conn.c    stream + map + pending fds + pump_wait
src/wayland/wire.c    str/array at word index
src/wayland/proto.c   relative-pointer opcode blob
src/wayland/map.c     id → kind/version
src/wayland/registry.c  get_registry + sync + globals
src/wayland/feedback.c  owned format table + tranche pairs
src/wayland/session.c bind + xdg + dispatch
src/vulkan/gpu.c        instance 1.4 + DRM match
src/vulkan/device.c     VkDevice, used 1.4 features, acquire timeline
src/vulkan/negotiate.c  feedback ∩ GPU modifiers (skip disjoint)
src/vulkan/swapchain.c  3 DMA-BUF images + per-image release timelines
src/engine/present.c    present_poll / begin / end
src/helper/drmfd.c      render node + SYNCOBJ_EVENTFD
```

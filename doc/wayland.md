# Wayland Wire Client (no libwayland)

`libwayland` is banned. The engine speaks the Wayland wire protocol in pure C23 and submits `SEND` / `RECV` / `RECVMSG` / `SENDMSG` on the compositor socket through raw `io_uring`.

## Files

| File | Role |
|---|---|
| `include/khoros/wayland/wire.h`, `src/wayland/wire.c` | Encode / decode headers, u32, i32, padded strings |
| `include/khoros/wayland/client.h`, `src/wayland/client.c` | UNIX socket connect, registry roundtrip, PBUF inbound, skip-success outbound |

## Wire format

Every message is 8-byte header plus payload, 32-bit little-endian:

```
uint32 object_id
uint16 opcode | uint16 size     /* size includes the 8-byte header */
```

Strings: `uint32` length including the NUL, then bytes, padded to 4. `khr_wl_pad4` is `(len + 3) & ~3U`.

Fixed object ids used during the first roundtrip:

| Id | Object |
|---|---|
| `KHR_WL_DISPLAY_ID` (1) | `wl_display` |
| `KHR_WL_REGISTRY_ID` (2) | `wl_registry` (created by `get_registry`) |
| `KHR_WL_CALLBACK_ID` (3) | `wl_callback` (created by `sync`) |

## Connect

`khr_wl_client_connect(client, ring, override_path)`:

- **100% Pure io_uring Direct Descriptors**: When `ring` supports registered files (default on Ring A via `khr_uring_config_ring_a`), socket creation and connection are executed entirely through `io_uring` in a single atomic submission without calling libc `socket()` or `connect()`.
- **Linked SQE Submission**:
  1. `khr_uring_prep_socket(ring, AF_UNIX, SOCK_STREAM, 0, KHR_DIRECT_SLOT_WAYLAND, true, KHR_TAG_WL_SOCKET)` linked with `IOSQE_IO_LINK`. The kernel allocates the socket directly into sparse slot `KHR_DIRECT_SLOT_WAYLAND` (slot 1), stripping `SOCK_CLOEXEC` to satisfy Linux 7.2 requirements for ring-private fixed files.
  2. `khr_uring_prep_connect(ring, KHR_DIRECT_SLOT_WAYLAND, &sun, sizeof(sun), true, KHR_TAG_WL_CONNECT)` with `IOSQE_FIXED_FILE`.
- **Zero POSIX FD Table Allocation**: The Wayland socket never enters the process's file descriptor table, eliminating `fget`/`fput` atomic refcounting and POSIX file table locks.
- **Direct Disconnect**: `khr_wl_client_disconnect` submits `khr_uring_prep_close(client->ring, client->sock_fd, true, KHR_TAG_CLOSE)` directly to the ring, closing the direct slot without libc `close(2)`.

## Roundtrip

`khr_wl_client_roundtrip` builds two requests in one buffer:

1. `wl_display.get_registry(new_id = 2)`
2. `wl_display.sync(new_id = 3)`

Submits `khr_uring_prep_send`, then loops `khr_uring_prep_recv` into `client->in_buf[16384]` until `wl_callback.done`. Both operations automatically apply `IOSQE_FIXED_FILE` when `client->is_direct` is true. Registry `global` events fill `client->globals[]` and cache shortcuts:

- `wl_compositor`
- `xdg_wm_base`
- `wl_shm`
- `wl_seat`
- `wp_linux_drm_syncobj_manager_v1`
- `zwp_linux_dmabuf_v1`

`khr_wl_client_roundtrip` executes with a bounded 2,000 ms timeout per io_uring stage to prevent hangs on disconnect. The protocol is verified via `test_wayland_client_mock_roundtrip` and `test_wayland_direct_socket_connect_and_roundtrip` over a local mock server, guaranteeing complete test coverage of both POSIX and pure direct-descriptor socket pipelines even when headless. Live compositor tests skip cleanly if the desktop socket cannot be opened.

Wire decoding (`khr_wl_decode_string`) enforces mandatory terminating NUL bytes, rejects integer overflows in length fields, and decodes 0-length nullable strings as empty strings (`""`), protecting downstream callers from invalid string memory access.

## PBUF inbound / tag-awaited outbound

After `khr_wl_client_attach_pbufs(client, tier0, tier1)` plus
`khr_wl_client_attach_topology(client, topo)`:

- `khr_wl_client_arm_inbound` — one multishot `RECVMSG` with `IOSQE_BUFFER_SELECT` on tier 0's `bgid`. `recv_hdr` stays in the client struct for the life of the request.
- `khr_wl_client_send_skip` / `khr_wl_client_send_with_fd` — one `SENDMSG` (completion REPORTED, never `SKIP_SUCCESS`), then `khr_topology_await_tag(topo, KHR_TAG_WL_SEND)` which pumps every pending completion through the normal classification and extracts ours. The exact byte count is required (`res == len` on our blocking socket; a short send is a hard failure, never a silent partial). One CQE per send, no linked barrier, zero completion debt by construction.
- `khr_wl_client_process_cqe(client, user_data, res, flags)` — **the steady-state consumer (non-blocking)**. The engine pumps Ring A (`khr_topology_pump` classifies into `evt_q`), then feeds each event here: `WL_RECV`/`WL_RECV_T1` packets resolve to their provided buffer, parse via `recvmsg_out` + the shared wire parser, and return a message count. Foreign tags and `-ENOBUFS` re-arm signals return 0. Buffer ownership stays with the caller (recycle with `khr_topology_recycle_cqe_buffer`). `khr_wl_client_roundtrip` is bootstrap-only (one registry discovery at connect, before `arm_inbound` — after arming, replies may land in PBUF instead of the one-shot receive, so mid-session roundtrips can time out on a healthy connection; steady-state sync uses an explicit `wl_display.sync` + pump).

Do not reuse `send_hdr` / `send_iov` / the caller's payload until the awaited SEND CQE arrives. Without an attached topology the sends fall back to consuming the next CQE blindly — sound only on a private ring with nothing else in flight (single-threaded tests); the awaited path is proven by `test_send_await_no_debt_no_steal` (inbound chatter guaranteed pending during the sends: all 15 bytes surface, zero SEND strays, exact ordered stream on the peer) and `test_await_tag_match_and_timeout`, including a runtime negative control (legacy blind wait fails the test deterministically).

History: the previous shape (`SKIP_SUCCESS` send + linked NOP barrier, blind next-CQE wait) confirmed the NOP, not the send — sound while quiet, but with armed inbound it could eat a RECV completion (false success + dropped protocol packet + send still in flight over a freed stack buffer). The mock's timing masked it; live Mutter killed us with `invalid arguments for wl_shm_pool.create_buffer`. The failure, the wire proof, and the fix are recorded in `muse.ai` (2026-09-07 send-await entry).

## DMA-BUF import (`wayland/dmabuf.h` / `gfx/dmabuf.h`)

`khr_dmabuf_image_init` allocates an exportable image (optimal-first tiling probe with linear fallback — the Xe stack only exports LINEAR), queries the real modifier back (INVALID on linear normalizes to LINEAR, never a hardcoded vendor value), reads pitch/offset, and `khr_dmabuf_image_export` hands out the DMA-BUF fd. `khr_dmabuf_import` runs params/add(+FD via `send_with_fd`)/create_immed as three sends. `test_dmabuf_wire_import_fd_passing` proves zero-copy by dev/ino identity across SCM_RIGHTS.

Live status on Mutter/Xe-experimental, revised 2026-09-07 after the send-await fix and the dmabuf present probe: with exactly-counted sends the import path works end to end — 16 GPU-rendered frames committed, every retired frame released exactly once, zero protocol errors. Mutter still never sends `created`/`failed` (silence, not refusal), so the loop treats `create_immed` buffers as usable immediately per protocol and tracks created/failed only as informational counters (both asserted on the mock, where the mock emits them). INVALID modifier is worse (total silence); hence the LINEAR normalization. The earlier "import stall" verdict was the send-await bug corrupting the add/params sends, not a compositor refusal.

## XDG shell (`xdg.h` / `xdg.c`)

Opcodes verified against `wayland.xml` + `xdg-shell.xml`; `xdg.h` is the single source (the mock includes it). Flow: `khr_xdg_bind` (batched registry binds) → `khr_xdg_create_toplevel` (surface, xdg_surface, toplevel, title/app_id, one empty commit — never a buffer) → `khr_xdg_consume` on the inbound stream: ping gets an immediate pong, `xdg_surface.configure` stores a pending serial (`khr_xdg_ack_pending` with the matching attach), toplevel configure stores w/h, close marks closed. `khr_xdg_can_attach()` is the attach gate: false until the first configure, false again after close. Proven live against Mutter and headlessly in `test_xdg_shell_lifecycle`.

## Product window (`window.h` / `window.c`, `core/config.h`)

`khr_window_run` is `make run`. Two DMA-BUF present slots, dirty-only commits. Client rect draws a BDA mesh (default unit box, or `./engine file.khrb` ingested async into the payload slice). Chrome cards live in the high 64 KiB of the hugepage; ingest cannot overwrite them.

Client-side decorations. Hit testing is a first-match rect table (`khr_hit_list_t`, `KHR_HIT_LIST_MAX` 16), not an if-ladder. Fill order: 16 px corners, 8 px edges, 32 px title-bar close, 32 px move bar. Miss is `KHR_HIT_CLIENT`. Exclusive fullscreen (F11) fills an empty list.

| Input | Action |
|---|---|
| Left-click close square | Stop the loop (same as SIGINT). Finger cursor. |
| Drag title bar | `xdg_toplevel.move` |
| Double-click title bar | `xdg_toplevel.set_maximized` / `unset_maximized`. Same request as dragging the window to the top of the output. The compositor picks the work-area size. Not `set_fullscreen`, not a pixel size we compute. |
| Drag edges / corners | `xdg_toplevel.resize` |
| F11 | Exclusive `set_fullscreen` |
| Esc | Dismiss cart, or leave exclusive fullscreen |
| Right-click client | Grabbing `xdg_popup` cart (shm). May hang outside the parent. Dismiss on `popup_done`, parent click, parent size change, Esc. |

`khr_window_hit` is a fill+pick wrapper for tests. The live loop fills and picks. Popup focus is `KHR_HIT_POPUP` from `pointer_surface`, not a parent-list rect.

## SHM present loop (`wayland/present.h`, first mapped window)

Double-buffered `wl_shm` present loop, the import-free road to the first window: one sealed memfd pool (`F_SEAL_SHRINK|F_SEAL_GROW` applied AFTER `ftruncate` — sealing first forbids the sizing and the pool creates at size 0), two `wl_buffer`s, round-robin commit, `wl_buffer.release` frees slots through `khr_present_consume()`. Attach only after `khr_xdg_can_attach()` (unconfigured attach is a fatal compositor error). `khr_present_paint_test` is the shared deterministic pattern (B = frame number) for tests and live runs.

Proven live against Mutter 2026-09-07 (/tmp probe, engine objects, no libwayland): configure received, 320x240 pool + 2 buffers accepted, 60 frames committed with 60 releases recycled, zero display errors, explicit `wl_display.sync` + pump alive-check passed, exit 0. `test_shm_wire_pool_and_buffer` (FD dev/ino identity across SCM_RIGHTS, seals asserted, oversize rejected pre-wire) and `test_present_loop_release_recycling` (round-robin, busy refusal, bogus/double release ignored) cover it on the mock through the same production paths.

## DMA-BUF zero-copy present + explicit sync (`wayland/dmabuf_present.h`, `gfx/dmabuf.h` slots)

The cutover from the shm path (ORIGINAL_REQUEST R2): two GPU slots, each an exportable LINEAR `B8G8R8A8_UNORM` image painted by Vulkan (dynamic rendering, card pipeline created once per slot, GENERAL export layout), exported once, imported once — no per-frame Wayland import churn. Every commit names an acquire point (our render completion) and a per-slot release point (compositor done); slots recycle on `wl_buffer.release`; `failed` retires a slot permanently.

Acquire bridge (`khr_dmabuf_present_sync`, stall-free by construction): the GPU signals the device timeline semaphore per frame; each present-loop lap queries the counter (`vkGetSemaphoreCounterValue`, an MMIO read, never a wait) and `TIMELINE_SIGNAL`s newly completed points into one long-lived DRM timeline syncobj imported once. Lag only delays display, never shows unfinished pixels. Release timelines (one per slot, host-created, imported once) advance per commit.

Why the bridge instead of Vulkan SYNC_FD export straight into the timeline? Probed live on Xe experimental (`tmp/sync_bridge_probe.c`): timeline SYNC_FD export fails at semaphore creation, and `FD_TO_HANDLE(IMPORT_SYNC_FILE)` on a real retired binary `sync_file` returns ENOENT — the fence→syncobj import is unsupported on this stack. The bridge uses only native syncobj ioctls proven green here. If a mature stack provides the direct bridge, it slots into `khr_dmabuf_present_sync()` without touching the wire loop.

Live proof on Mutter 2026-09-07 (`tmp/dmabuf_present_probe.c`): 16 frames / 15 releases (the 16th still displayed — correctly unreleased), acquire pushed through point 116, zero display errors, alive-check passed, exit 0. Acquire-gating proof: a commit naming a future point no submission signals stalled 2.5 s with zero releases (fully rendered work, valid commit, no error) — the compositor waits on our timeline; it released exactly on supersede. Tests: `test_dmabuf_present_created_failed_consume` (pure wire, no GPU), `test_dmabuf_present_loop_mock` (exact 13-send init accounting, points, round-robin, busy refusal, retire-on-destroy), `test_dmabuf_present_acquire_bridge` (GPU completion → DRM point via non-blocking query).

## Not yet

- `zwp_linux_dmabuf_v1` feedback (format/modifier negotiation; currently LINEAR ARGB8888 by probe, no negotiation)
- GPU-side release-wait import (today slot reuse gates on `wl_buffer.release` events; importing the release fence as a Vulkan wait-semaphore for full pipelining is future work)
- SCM_RIGHTS receive (needs `msg_controllen` + tier 1) and `IORING_OP_FIXED_FD_INSTALL` on the send path
- Replacing the roundtrip `IORING_OP_RECV` loop with the armed multishot path

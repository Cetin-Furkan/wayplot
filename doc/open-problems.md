# Open problems (2026-09-07 Grok pass)

Working list after a full-tree read. Evidence is the source, the latest on-disk logs, and `tmp/` live probes — not the test-count banners.

Do **not** start DMA-BUF format/modifier feedback, GPU-side release-wait import, `VK_KHR_display`, or plot-shader present until P0–P2 are done. Those are the next capabilities. They are not the current hole.

Status this note was written against:

- Branch `engine` @ `5fc076e`, dirty worktree.
- Latest native log: `logs/test_logs/test_20260907_190058.log` — **93/93, 2005 assertions, ~2.6 s**.
- Latest sanitize: `logs/test_logs/sanitize_results.log` — **93/93, ~20.5 s**, 124 ASan leaks in `<unknown module>` suppressed.
- Host: CachyOS `7.3.0-rc1`, i7-1165G7, Mutter `wayland-0`, `/dev/dri/card0` + `renderD128`, Xe experimental, `HugePages_Total = 0`.
- `make run` / `engine_init()`: topology smoke only. Live window: `tmp/dmabuf_present_probe.c` (gitignored).

Agent memory: append-only [`../grok.ai`](../grok.ai), [`../muse.ai`](../muse.ai), [`../gemini.ai`](../gemini.ai). This file is the punch list; those files are the diary.

---

## P0 — Freeze the win

The R2/R3 stack is uncommitted. If the worktree is lost, the live window is lost.

| Item | Fact |
|---|---|
| HEAD | Topology rework (cores 2/3, parked worker, pump, offscreen frame) is in `5fc076e`. |
| Dirty | 16 modified files (~617 lines): send-await, mock FD attach, runner, `wayland.md`, `TEST_READY.md`, `muse.ai`. |
| Untracked | 20 C/H files (~3500 lines): `xdg`, `shm`, `present`, `wayland/dmabuf`, `syncobj`, `gfx/dmabuf`, `dmabuf_present`, plus tests. |

Atomic commits per [`GITHUB.md`](../GITHUB.md). Never `git add .`. Suggested cut points: send-await → XDG → SHM present → DMA-BUF wire + gfx export → syncobj wire → DMA-BUF present loop → doc truth.

Keep `tmp/` gitignored. Probes are tools, not the product.

---

## P1 — The engine binary is not the engine

`src/engine.c` prints the banner, `khr_topology_init`, MSG_RINGs `(uint64_t)topo.hugepage` (a **host pointer**, not a `VkDeviceAddress`), destroys. Zero Wayland, zero Vulkan, zero present. `main.c` only calls that.

The composed loop already exists in `tmp/dmabuf_present_probe.c`:

```
topology → connect → roundtrip → attach pbuf+topology
        → xdg bind/toplevel → arm inbound → pump
        → gfx device + BDA arena
        → dmabuf_present_init → commit frames
        → consume release/created/failed → alive-check
```

Promote that into `engine_init` / a persistent `engine_run` that `make run` actually drives. A mapped Mutter window is the acceptance test for this item. Until then every green suite can lie about integration.

On the way in (cheap, do not defer):

- Handle `wl_display.error` in the production pump. Probes dump it; the engine currently would not.
- Keep `create_immed` buffers usable immediately. Mutter silence on `created`/`failed` is a compositor quirk, not a stall.
- Single inbound dispatcher: `khr_wl_client_process_cqe` today only understands registry `global` + callback `done`. XDG/present tests parse payloads themselves. The product loop needs one consume path.

---

## P2 — Data-path honesty (after a window is in `make run`)

| Item | Fact | Fix |
|---|---|---|
| Ingest arena ≠ shader BDA | `khr_bda_arena_init_with_host_ptr` exists. Nothing passes `topo->hugepage`. Two 2 MiB mappings. `test_ipc_ingestion_to_bda_pipeline` **simulates** a device address (`0x2000'0000 + host ptr`). | Wrap the topology hugepage as the Vulkan BDA arena. |
| Present thread blocks on disk | `khr_topology_ingest` waits up to 2 s on Core 2. DMA already runs on Ring B; the waiter is the violation of “presentation never stalls for disk.” | Non-blocking ingest: push + doorbell, reap MSG_RING via the pump. |
| `acquire_fd` mislabeled | Device creates a timeline with `OPAQUE_FD`, exports `d->acquire_fd`, present never uses it. Header calls it a DRM syncobj fd. Present uses a **separate** native DRM timeline + counter-query bridge. | Either drop the unused export, or keep it as an optional mature-stack seam. Do not claim SYNC_FD until Xe (or a second GPU) actually exports it. |
| Xe bridge | Timeline SYNC_FD create fails; `FD_TO_HANDLE(IMPORT_SYNC_FILE)` → ENOENT (`tmp/sync_bridge_probe.c`). Bridge is `vkGetSemaphoreCounterValue` + `DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL`. Correct for this box. | Keep the seam in `khr_dmabuf_present_sync()`. Do not pretend R2’s letter is satisfied. |

---

## P3 — Correctness nits (fold into P1/P2, not a cleanup holiday)

### Color pack (`src/gfx/dmabuf.c` `khr_dmabuf_slot_render`)

`shaders/card.slang` `unpack_rgba`: R = low byte, A = high byte. Frame test packs `0xFF0000FF` and is consistent (red center pixel passes). Slot render packs `(r<<24)|(g<<16)|(b<<8)|255` — R in the high byte, A in the low byte. Live cards still “animate.” Channels and alpha are wrong vs intent.

Pack the same way the shader and the frame test already do.

### Send path name

`khr_wl_client_send_skip` does **not** set `IOSQE_CQE_SKIP_SUCCESS`. It awaits `KHR_TAG_WL_SEND` for the exact byte count. Rename when that function is touched. `architecture.md` still describes the old SKIP_SUCCESS + NOP barrier shape.

### `await_tag` is not O(1)

`topology.h` says the pump is O(1) with no memmove. `khr_topology_await_tag` scans `evt_q` and compact-memmoves. Harmless at current sizes; the comment is false.

### PBUF stream reassembly

`*_consume` drops a truncated tail. UNIX stream + 256 B tier-0 can split a Wayland message. Roundtrip’s 16 KiB `in_buf` hides this. The armed multishot path will not hide it forever.

### Syncobj create flags

`khr_dmp_create_timeline` and the DRM tests use `flags = 0`, never `DRM_SYNCOBJ_CREATE_TIMELINE`. This kernel still accepts `TIMELINE_SIGNAL`. That is not the documented create-timeline API.

### Ingest `open_how` lifetime

Async `khr_ingest_chain_submit` keeps `struct open_how` as a stack local and returns. Safe only because `DEFER_TASKRUN` + `GETEVENTS` on submit copies before return. That is a kernel-timing invariant, not an API one. Move it into the worker-owned op state.

### Partial ingest SQE submit

If SQE 1 or 2 cannot be allocated, the prefix is unlinked and submitted. OPENAT2 can land in slot 0 with no CLOSE. Worker then retries into the same slot.

### Outstanding counters on timeout

`wait_bda` / `ingest` increment outstanding on push. Timeout does not decrement. Later a tier-1 packet of size `0xBDA0` / `0x1E57` can be misclassified. Tier-0 cannot hit those sizes (256 B cap).

### Frame path vs slot path

`khr_gfx_frame_render_red_card` rebuilds the card pipeline every submit and host-waits the timeline (test-only stall). Slots create the pipeline once and never host-wait. Do not copy the frame path into a hot loop.

### Device header unions

`phy`/`physical_device`, `gfx_queue`/`graphics_queue`, etc. are test-compat aliases. Do not grow more. Pick one name when a TU is already open.

---

## P4 — Documents that contradict the tree

Update the manual that matches the code you are touching. Do not rewrite agent logs.

| File | Lie |
|---|---|
| `doc/architecture.md` | Pins drawn as CPU 0/1 (code is 2/3). Outbound still `SENDMSG` + `SKIP_SUCCESS`. “What does not exist yet” still lists Vulkan BDA, Slang, syncobj acquire/release, DMA-BUF. Test count 81. |
| `doc/graphics.md` | Index in `doc/README.md` still says “not implemented yet.” Intended path still says “push-descriptor bind.” “Tests must skip without GPU” is false for most of Suite 5. |
| `doc/build.md` | “Engine has no `-lvulkan`.” Makefile links it. Toolchain line still 7.2-rc7. |
| `doc/testing.md` | Frozen at early topology; ASan “32/32, 913 asserts.” No Suite 5–7 of frame/dmabuf-present. |
| `TEST_INFRA.md` | Still 71 tests. DMA-BUF mapped to stride math + mock memfd. |
| `TEST_READY.md` | Addenda 81→82→84→90→93 match the runner. The frozen 71/71 dump and “make test = 71 tests” do not. Leave history; fix the command blurb. |
| `README.md` | Presentation advertised as `VK_KHR_display`. Live path is io_uring Wayland + DMA-BUF. |
| `include/khoros/gfx/device.h` | `acquire_fd` comment: “Exported DRM syncobj file descriptor.” It is `OPAQUE_FD`. |

`doc/wayland.md` and `doc/topology.md` are current. Prefer them when they disagree with `architecture.md`.

---

## P5 — Later. Not now.

These are real. They are not the next edit.

- `zwp_linux_dmabuf_v1` get_default_feedback / get_surface_feedback (format/modifier negotiation). This box presents LINEAR ARGB8888 by probe.
- Import compositor release as a Vulkan wait-semaphore (pipelining beyond `wl_buffer.release`).
- `VK_KHR_display` / KMS direct-to-display. Different product from ORIGINAL_REQUEST R2.
- Plot pipeline in the present loop. Compiled, `#embed`’d, tested, unused. A window of cards is the proof.
- Cold-path `.obj` / `.dem` → `.wpbin`.
- `wl_seat` / input. Registry caches the global; nothing binds it.
- Live resize of DMA-BUF slots. `test_tier4_window_resize_reconfiguration` is mock-level ack + timeline destroy, not a buffer rebuild.
- Replacing bootstrap `roundtrip` one-shot `RECV` with the armed multishot path.
- SCM_RIGHTS receive (`msg_controllen` + tier 1).
- GitHub Actions `ubuntu-latest` + gcc-14 reproducing 93/93. Hardware-gated by design. Do not weaken tests to make CI green.

---

## Test-suite honesty

93/93 on this box is a **local hardware fact**. It is not “the dual-thread present loop is proven.”

| What | What it actually asserts |
|---|---|
| `test_dmabuf_present_loop_mock` / `_acquire_bridge` | Real GPU + mock compositor through production `khr_dmabuf_present_*`. This is the real E2E. |
| `test_gfx_real_submit_and_readback` | Real submit + red center pixel. Honest SKIP-as-PASS without a GPU. |
| `test_send_await_*` | Tag-await vs the old blind wait. Negative control included. ~2 s because of a timeout case. |
| `test_tier3_*` / `test_tier4_*` | Hand-encoded mock wire. memfd pretending to be dma-buf/syncobj. “144 Hz” is 3×1 ms `nanosleep`. Keep; do not cite as present-loop proof. |
| `test_ipc_ingestion_to_bda_pipeline` | Ingest into hugepage, then a **fabricated** 64-bit value. |
| Suite 5 without `/dev/dri` | Most tests FAIL cleanly (`TEST_ASSERT` on device init). Frame/present SKIP-return-true and count as PASS. Docs disagree with each other about which policy is intended. |
| `make sanitize` 93/93 | lsan suppressions for `libvulkan` / `libvulkan_intel` / `libdrm` / `<unknown module>`. Last log: 124 leaks / 5673 B. Not “gfx heap clean.” |

Earlier the same day, `test_20260907_185832.log` was **2/93 FAIL** on dmabuf present commit. The path is hardware-sensitive. Treat a red present test as a real red, not a flake to retry into green.

There is no SKIP status in the runner. `printf("(SKIP…)")` + `return true` is a PASS.

---

## Load-bearing facts (do not re-learn)

These bit us live. They live in `kernel-notes.md` and `wayland.md` as well. Repeated here so a session that only opens this file still sees them.

1. Await `KHR_TAG_WL_SEND` through the pump. Blind next-CQE + `SKIP_SUCCESS` ate RECV completions. Mutter: `invalid arguments for wl_shm_pool.create_buffer`.
2. Memfd: `ftruncate` then `F_SEAL_SHRINK|F_SEAL_GROW`. Sealing first freezes size at 0.
3. Xe DMA-BUF: LINEAR only. `DRM_FORMAT_MOD_INVALID` on linear must become `LINEAR` or Mutter is silent.
4. `create_immed` buffers are usable immediately. Mutter never sends `created`/`failed`. Silence is not refusal.
5. `IORING_REGISTER_CLOCK` `nr_args` is 0. Unregister ring fds before `close`. `DEFER_TASKRUN` needs `GETEVENTS`. Ring B is created on the worker (`SINGLE_ISSUER`). Direct `SOCKET` must not pass `SOCK_CLOEXEC`. `64'536 == 64536`; 64 KiB is `65'536`.
6. `make lint` greps `poll(`. Do not name functions `*_poll(`.
7. Topology tests that call `khr_topology_init` must destroy even on failure, or the worker is stranded on freed stack.

---

## Acceptance for the coming fix pass

Done means all of:

1. Worktree checkpointed (P0), probes still gitignored.
2. `make run` maps a DMA-BUF window on Mutter with explicit-sync points, no protocol error, clean disconnect.
3. Slot RGBA matches the shader / frame test.
4. `wl_display.error` is observed by the product loop.
5. Manuals that currently contradict the tree (`architecture.md`, `graphics.md`, `build.md`, `testing.md`, `README.md`) describe what exists.
6. `make lint` + `make test` + `make sanitize` still green **on this hardware**. Do not claim 93/93 on GPU-less CI.

Not done: feedback negotiation, release-as-GPU-wait, KMS, plot present, cold path, input.

# Developers and agents

This file is the how. The why is [README.md](README.md). Subsystem manuals live in [doc/](doc/README.md). Agent session log is [grok.ai](grok.ai) (append-only).

The public name is **Wayplot**. The C tree is **Khoros**. Same program.

---

## Machine

- Linux, **kernel 7.2+** (CachyOS is the development host). GitHub `ubuntu-latest` is not that kernel.
- GCC with ISO C23 (`-std=c23`) or a Clang that actually implements it
- Vulkan 1.4, `pkg-config` for `vulkan` and `libdrm`
- `slangc` for `shaders/*.slang`

## Commands (the only list)

```bash
make              # ./build/engine
make test         # ./build/test_runner  →  logs/test_logs/
make lint         # banned headers / libwayland / liburing / epoll / poll
make sanitize     # ASan + UBSan
./build/engine
./build/engine path/to/file.khrb
./build/engine --write-box /tmp/box.khrb
```

Export a mesh (engine does **not** read `.blend`):

```bash
blender --background --python tools/export_khrb.py -- /tmp/mesh.khrb
blender --background --python tools/export_khrb.py -- --monkey /tmp/suzanne.khrb
./build/engine /tmp/suzanne.khrb
```

Present is **dirty-only**. Do not “fix” idle by redrawing every frame.

Publish: commit on `engine`, push when the human asks. Do not invent a second remote dance. Do not cargo-cult commit one-liners from old notes.

---

## What the tests actually cover

They are not a scam and they are not the product.

They cover wire, io_uring, topology, ingest into the payload, hit lists, KHRB parse, camera pitch clamp, shader modules, mock compositor. They do **not** cover “Suzanne faces the camera,” lighting that reads as a cube, or color. A green bar after a math change is not permission to skip looking at the window.

If you change camera, Y-flip, winding, or cull, add a *contract* test (e.g. default frame puts object +Z toward the camera). Do not add a test that loads a monkey file and asserts nothing about facing.

---

## Design choices that are not optional

| Do | Do not |
|---|---|
| ISO C23 keywords (`bool`, `nullptr`, `alignas`, `constexpr`) | `<stdbool.h>`, `<stdalign.h>` |
| Raw `io_uring` syscalls | `liburing`, `epoll`, `poll` |
| Wayland wire in this tree | `libwayland` |
| DMA-BUF slots + DRM syncobj | Vulkan WSI, `VkSwapchainKHR`, `VK_KHR_display` as the window |
| Hugepage payload `[0, 2MiB-64KiB)`, UI in the high 64 KiB | Ingest into the title-bar cards |
| GPU reads verts through BDA | CPU rebuild of the mesh every frame |
| Backface cull + depth on the GPU | CPU list of “visible” faces |
| Native `.khrb` | Parsing `.blend` in the engine |
| Split fat files as **siblings** | Deeper folders “for architecture” |

Present thread does not wait on disk. Ring B ingests. Ring A presents. MSG_RING is the 8-byte pointer, not a copy of the file.

---

## Window, today

Client-side decorations. Hit list is first-match (`config.h`), not an if-ladder. Close is a title-bar square; NE resize is 3×3 so it does not steal close. Gimbal is in the **client**, below the 32 px bar, inset so it misses the 8 px east strip.

| Input | Action |
|---|---|
| LMB drag (client or gimbal) | Turntable orbit |
| Shift+LMB or middle-drag | Pan |
| Wheel | Zoom |
| F or double-click client | Frame, keep yaw/pitch |
| Click gimbal arm / hub | Snap look-from ±X ±Y ±Z |
| Double-click gimbal | Reset orient + frame |
| Double-click title bar | `xdg_toplevel.set_maximized` |
| F11 | Exclusive fullscreen |
| Title-bar close / Esc as documented | Leave |

---

## Layout

```
include/khoros/   public headers (core, gfx, uring, wayland)
src/              matching .c
shaders/          Slang; SPIR-V is generated under build/
tools/            cold path only (export_khrb.py)
tests/            plumbing tests
doc/              manuals (not the landing page)
```

`window.c` still does too many jobs. Split by job at the **same** depth when you split. Do not nest.

---

## Improve it

1. Read README (what this is) and this file (how).
2. Read `doc/open-problems.md` and `doc/kernel-notes.md` before touching rings.
3. Change one job. Look at the window if the job is pixels. `make test` and `make lint`.
4. Do not claim a visual in a test that never rendered. Do not grow the hugepage “while we are here.” Do not start a font foundry or a CAS in a drive-by.

The next product slice is a **field you can see and scrub**, not a deeper window and not a text engine. See the README.

# wayplot

**Reinvention of the wheel for Linux graphics.**

A from-scratch Wayland + Vulkan 1.4 host for scientific and engineering 3D on **Mesa iGPUs** (Intel ANV, AMD RADV; Turnip later). Office laptops. Shared RAM. Not NVIDIA-first, not GLFW, not libwayland.

The product is a **document + views + GPU visualization**: meshes, heightmaps, plots, and instrument cards on one swapchain. The GPU shades triangles the CPU placed. It does not own the model.

---

## Caution

This repository is written **with** an AI agent. There is no clean line between “human code” and “AI code” here. Planning, structure, and the C are mixed. That is intentional, not a footnote.

---

## Why this exists

Almost nothing talks to an iGPU as if it were a first-class machine. On Linux, BSD, and Windows the default path is still: stack a program on a toolkit on a library on twenty-year-old code, keep it compatible with kernels nobody should be targeting, and ship. Nobody wants to reinvent the wheel. That is how we stay stuck. New hardware is lucky if we use half of it.

3D on Linux is worse. The engineering and CAD tools people actually run sit on code designed for one core and GPUs that no longer exist. NVIDIA did start over, and they are good at it. I will not pretend otherwise. Because they became the GPU that “just works,” everyone else stopped caring: labs buy NVIDIA, desktops that sound like hair dryers, and anyone with a thin office laptop is told they cannot do the job.

Wayplot is the opposite bet. A skeleton for programs that should run **smooth** on the machine you already carry: lowest latency I can measure, as little RAM / CPU / GPU as the work needs, shared-memory paths on iGPU instead of a fake PCIe upload story.

Wayland still waits with `poll` on a Unix socket. I dropped libwayland and speak the wire on **io_uring** (recv and send), so the compositor client is not a pile of extra syscalls and lock convos. Vulkan WSI also hid libwayland behind it, so present is DMA-BUF and drm syncobj on that same ring — not their windowing engine. The same shape is what you would want if you were writing a game that has to feel honest at 240 Hz.

The goal is not a demo cube. It is a host I will keep until it is a scientific engine I can use myself: first a flood / hydrology model (compute + this display), then concrete formulas with a physics model in the loop, results on the same viewport. Those two are the first real applications.

Using AI for design and for code is not a taboo. It is how this will be written. People should see that.

Create something nice today.  
Someone very curious.

---

## For AI agents

Start at [`AGENTS.md`](AGENTS.md) in this directory. Architecture that is not in the code: [`docs/HOST.md`](docs/HOST.md), [`docs/CONTEXT.md`](docs/CONTEXT.md). Step contracts live in `docs/` (`DOC.md`, `PLAN.md`, `GRID.md`, `GROUND.md`, …).

---

## Status

`./bin/normal/wayplot` opens an xdg window:

- **Two instrument cards** (CPU `x,y,w,h` + captions: what is loaded, plot `n` / min / max).
- **Two panes**, one document, one opaque pass then one overlay. Left is perspective 3D. Right is an **orthographic plan** (map). Drag on the plan rotates the map; it does not become a second 3D view.
- A world **XZ grid**, the cube (or `--mesh` / `--dem`), a plot ribbon, optional `--image` (drape on a DEM, otherwise a ground under the scene — not a slab through the cube).

Close the window to exit. Tile / maximize / fullscreen rebuilds the swapchain at logical × integer scale.

```text
./bin/normal/wayplot
./bin/normal/wayplot --version
./bin/normal/wayplot --mesh FILE.obj
./bin/normal/wayplot --dem FILE.pgm
./bin/normal/wayplot --plot FILE.txt
./bin/normal/wayplot --image FILE.ppm
```

Bad mesh / DEM / plot / image prints an error; the window still opens.

Left mouse orbits the 3D pane (yaw only on the plan). Right or middle pans. Scroll dollies. Cards drag. Window edges resize.

---

## Build

```text
make                  # quiet (normal)
make debug            # [debug] logs on stderr
make release          # -O2; MINOR++ if main.c or src/**/*.c changed
make release update   # MAJOR++  (0.2.0 -> 1.0.0)
make optimize         # -O3 -march=native -flto; this machine only
make asan
make test
```

Binary: `bin/<normal|debug|release|optimize|asan>/wayplot`

`make test` builds the binary and runs the suite (uring, Wayland proto/conn/registry/session, Vulkan pick/device, present, math, mesh/OBJ/DEM/plot/grid/image, SPIR-V layout, raster, font/text, pass, draw list, card, hit, view, doc, bench). GPU tests lock pixels, not screenshots. Logs in `build/test-*.log`. `test/` does not bump the version. Make does not tag GitHub.

Needs: C23 (GCC 13+ / Clang 16+), Vulkan 1.4 headers, libdrm headers, FreeType 2, Linux `io_uring` UAPI, a TTF at one of DejaVu / Noto Sans / Liberation Sans. A live Wayland compositor for the GPU tests.

---

## Layout

```text
main.c          entry (stays at the repo root)
src/            helper uring wayland vulkan engine renderer
include/        same names as src/
shaders/        Slang techniques
test/           same names as src/
docs/           decisions that are not in the code
bin/            linked binaries
build/          objects (gitignored)
```

Split a directory when a **file** is fat. Do not add `init/`, `begin/`, `pick/` folders for one function.

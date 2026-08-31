# wayplot

Wayplot is a new graphical engine for scientific and engineering 3D simulations and tests that is build completely from scratch using Vulkan and Wayland on Linux enviroment.

Wayplot is specifically optimized for relatively modern laptop devices that utilize iGPUs, this repo has no NVIDIA specific codes or optimizations.

A from-scratch Wayland client with Vulkan 1.4 on **Mesa iGPUs** (Intel ANV, AMD RADV; Turnip later). Scientific 3D + instrument cards on office laptops. Not a NVIDIA stack, not GLFW, not libwayland.

This tree is a new host. The previous client (DMA-BUF, feedback, io_uring recv) lives next to this folder as reference — copy algorithms, do not re-type them from memory.

## Build

```text
make                  # quiet (normal)
make debug            # [debug] logs on stderr
make release          # -O2; MINOR++ if main.c or src/**/*.c changed
make release update   # MAJOR++  (0.2.0 -> 1.0.0)
make optimize         # -O3 -march=native -flto; this machine only
make asan
```

Binary: `bin/<normal|debug|release|optimize|asan>/wayplot`

`make test` builds and runs ring, math, mesh, SPIR-V layout, raster, font, text, pass, draw list, **card**, proto, conn, live registry, session/feedback, GPU pick, present, cube, and the msg/s bench. Logs in `build/test-*.log`. `test/` never participates in versioning. No GitHub release from Make.

```text
./bin/normal/wayplot --version
./bin/debug/wayplot
```

Needs: C23 compiler (GCC 13+ / Clang 16+), Vulkan 1.4 headers, libdrm headers, FreeType 2, Linux `io_uring` UAPI. A TTF at one of DejaVu / Noto Sans / Liberation Sans.

## Layout

```text
main.c          entry (stays at the repo root)
src/            helper uring wayland vulkan engine renderer
include/        same names as src/
shaders/        Slang techniques when they exist
test/           same names as src/
docs/           decisions that are not in the code
bin/            linked binaries
build/          objects (gitignored)
```

Split a directory when a **file** is fat. Do not add `init/`, `begin/`, `pick/` folders for single functions.

## Status

`./bin/normal/wayplot` opens an xdg window: a lit cube, **two** overlay cards (`x,y,w,h` on the CPU), and a label, recorded from a CPU draw list into one opaque pass then one overlay pass. Tile, maximize, or fullscreen — the swapchain rebuilds at logical × integer scale. Close the window to exit.

Architecture (why the host is a document + views, not a Slang world): `docs/HOST.md`.

Step docs: `docs/PRESENT.md`, `docs/CUBE.md`, `docs/TEXT.md`, `docs/RESIZE.md`, `docs/PASSES.md`, `docs/LIST.md`, `docs/CARD.md`, `docs/CARDS.md`.

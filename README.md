# graphway

**High-performance, low-power interactive 2D/3D graphing library for modern Linux**  
*C23 + Wayland + Vulkan 1.4*

A specialized, non-bloated replacement for matplotlib built from the ground up for **beautiful visuals**, **smooth interaction**, and **near-zero CPU usage when idle**.

> **License**: This project is released into the **public domain** under the Unlicense.  
> You are free to use, modify, distribute, sell, or do anything you want with this code — **with or without attribution**.

---

## Features

- **True low-power design** — Blocking event loop + dirty-flag rendering + `VK_PRESENT_MODE_FIFO_KHR` → ~0% CPU when the graph is static
- **Modern Wayland + Vulkan 1.4** — Full use of latest protocols (xdg-shell, fractional-scale, presentation-time, explicit sync via drm syncobj) and Vulkan 1.4 features
- **Beautiful by default** — Crisp HiDPI rendering, clean typography, professional color palettes, smooth pan/zoom/rotate
- **Written in clean C23** — Highly optimized, minimal dependencies, compiler-friendly code
- **Full control** — You own the entire rendering and event pipeline (no hidden Python or OpenGL overhead)
- **Python-ready** — Stable public C API designed for easy `ctypes` wrapping and future native Python extension
- **Specialized for scientific visualization** — Focused exclusively on interactive 2D & 3D graphs (line, scatter, bar, surface, etc.)

---

## Why graphway?

matplotlib is powerful but slow, dated-looking, and surprisingly CPU-heavy even for static plots. Most modern alternatives still rely on Python loops or older graphics APIs.

**graphway** takes a different approach:

- We speak directly to the compositor via Wayland and the GPU via Vulkan 1.4.
- We only do work when something actually changes.
- We leverage the latest kernel improvements and explicit synchronization for maximum efficiency.

The result is a graphing library that feels native, looks modern, and respects your system resources.

---

## Current Status

**Phase 0 — Foundation Complete** (July 2026)

- Professional project structure
- Advanced Makefile with protocol generation, shader compilation, debug/release/sanitizer builds
- Rich error handling framework and logging system designed
- Public C API header defined
- Unlicense (public domain)

**Next**: Phase 1 — Basic Wayland window + Vulkan 1.4 surface + clearing render loop with full error handling and true idle behavior.

The project is in active early development. Core infrastructure is being built carefully before adding graph features.

---

## Technical Highlights

### Low-Power Architecture
- Wayland event loop blocks in the kernel when idle
- Rendering only occurs when the internal `dirty` flag is set
- FIFO present mode allows the compositor and GPU to power down between frames
- Explicit sync support (via modern Wayland + Vulkan extensions) for optimal driver behavior on recent kernels

### Error Handling
Every Vulkan and Wayland call is protected. We use a rich `GraphwayResult` enum with specific error codes, detailed logging (file + line), and graceful recovery paths (e.g. swapchain recreation on `VK_ERROR_OUT_OF_DATE_KHR`).

This makes development fast and the final library robust for long-running applications and Python bindings.

### Technology Stack
- **Language**: C23 (clang 22+)
- **Display**: Wayland (1.25+) with xdg-shell, presentation-time, fractional-scale-v1, linux-drm-syncobj-v1
- **Graphics**: Vulkan 1.4 (headers 1.4.350+)
- **Shaders**: GLSL compiled to SPIR-V via glslang / shaderc
- **Build**: Custom Makefile with LTO, sanitizers, and dependency tracking

---

## Building

### Prerequisites (Arch Linux example)

```bash
sudo pacman -S wayland wayland-protocols vulkan-icd-loader vulkan-headers \
               vulkan-validation-layers mesa clang pkgconf make glslang shaderc
```

### Build Commands

```bash
git clone https://github.com/Cetin-Furkan/graphway.git
cd graphway

make              # Debug build (sanitizers enabled)
make release      # Optimized production build (-O3 -flto -march=native)
make clean
make format       # Run clang-format
make tidy         # Static analysis with clang-tidy
```

The Makefile automatically generates Wayland protocol code and compiles shaders.

---

## Usage (Planned API)

```c
#include <graphway/graphway.h>

GraphwayContext* ctx = NULL;
if (graphway_create("My Plot", 1280, 720, &ctx) != GRAPHWAY_SUCCESS) {
    // handle error
}

graphway_add_line_series(ctx, x_data, y_data, count, 0xFF00AAFF, 2.0f);
graphway_set_view_limits(ctx, 0, 10, -1, 1);

graphway_run(ctx);           // Blocking event loop (pan, zoom, close)
graphway_destroy(ctx);
```

A high-level Python API via `ctypes` (and later a native extension) will be provided.

---

## Roadmap

| Phase | Status          | Description |
|-------|------------------|-------------|
| 0     | ✅ Complete     | Project foundation, error system, structure, Makefile, Unlicense |
| 1     | In Progress     | Wayland client + Vulkan 1.4 surface + basic window + idle render loop |
| 2     | Planned         | 2D graph rendering (lines, scatter), pan/zoom interaction, dirty flag system |
| 3     | Planned         | Text rendering, axes, grids, legends, tooltips, professional styling |
| 4     | Planned         | 3D graphs, camera control, depth buffering |
| 5     | Planned         | Python bindings + numpy integration + examples |
| 6     | Planned         | Polish, performance tuning, multiple figures, export |

---

## Contributing

This project is being developed in a structured, teaching-oriented way. We prioritize:

- Clean, well-documented C23 code
- Excellent error handling and logging
- True low-power behavior
- Minimal dependencies and bloat

If you want to help, the best way right now is to follow development, test on different compositors/GPUs once Phase 1 lands, and give feedback on the public API design.

---

## License

**Unlicense** — Public Domain

This project is released into the public domain. You can do **anything** you want with the code:

- Use it in commercial projects
- Modify it however you like
- Redistribute it (with or without changes)
- Sell it
- Remove all traces of the original author

**No attribution is required.**

See the [LICENSE](LICENSE) file for the full legal text.

---

## Acknowledgments

- The Wayland and Vulkan communities for building excellent modern APIs
- All the developers working on explicit synchronization and power-efficient graphics on Linux
- Everyone who wants better scientific visualization tools on Linux

---

**graphway** — Because your graphs deserve to look good *and* run efficiently.

---

*This README is part of the initial public release of the project foundation (July 2026).*

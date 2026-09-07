# Khoros Engine

A high-performance systems engine engineered exclusively in pure **ISO C23** for **Linux kernel 7.2+ (CachyOS)**.

## Core Directives

- **Language Standard**: ISO C23 (`-std=c23`). No C++, no C99/C17 idioms.
- **Banned Headers**: `<stdbool.h>` and `<stdalign.h>` are strictly prohibited. Keywords `bool`, `true`, `false`, `alignas`, `alignof`, `nullptr`, `constexpr`, and `auto` are used natively.
- **I/O Subsystem**: Direct kernel `io_uring` through raw syscalls (`io_uring_setup`, `io_uring_enter`, `io_uring_register`). Zero `liburing`, zero `epoll`, zero `poll`.
- **Graphics Pipeline**: Pure Vulkan 1.4 (`VK_API_VERSION_1_4`) with Synchronization 2. Zero `libwayland`, zero OpenGL. Presentation via KMS/DRM Direct-to-Display (`VK_KHR_display`) and offscreen compute.

## Quickstart

### Prerequisites
- Linux kernel 7.2+ (e.g. CachyOS)
- GCC 15+ or Clang 18+ with complete C23 standard support
- Vulkan SDK 1.4+

### Build & Run
```bash
# Build the engine
make

# Run the engine
make run

# Clean build artifacts
make clean
```

## Documentation

Subsystem manuals are in [`doc/`](doc/README.md): architecture, C23 rules, io_uring, PBUF, topology, Wayland, ingest, testing, build, graphics roadmap, and kernel 7.2 notes.

Agent memory (append-only, never rewrite): [`grok.ai`](grok.ai), [`gemini.ai`](gemini.ai).

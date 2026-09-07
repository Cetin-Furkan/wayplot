# Developer Guide: Khoros Engine

Welcome to the Khoros Engine developer documentation. This document details our development environment, build practices, architectural patterns, and strict coding conventions.

---

## 1. System Requirements

- **Operating System**: Linux kernel 7.2+ (Optimized for CachyOS / BORE scheduler)
- **Compiler**: GCC 16+ or Clang 22+ with ISO C23 standard conformance (`-std=c23`)
- **Graphics / Compute**: Vulkan SDK 1.4+ (Vulkan 1.4.357+) with DRM/KMS Direct-to-Display support
- **Kernel Headers**: Modern `<linux/io_uring.h>` containing registered ring, cooperative taskrun, and futex operations

---

## 2. Strict C23 Idioms & Banned Items

All code contributed to Khoros must strictly adhere to ISO C23. We enforce this through both preprocessor guards and compiler flags:

### 2.1 Prohibited Headers & Replacements
| Prohibited Item | Reason | C23 Replacement |
|---|---|---|
| `<stdbool.h>` | Obsolete in C23; `bool`, `true`, `false` are built-in keywords | Built-in keywords: `bool`, `true`, `false` |
| `<stdalign.h>` | Obsolete in C23; `alignas`, `alignof` are built-in keywords | Built-in keywords: `alignas`, `alignof` |
| `NULL`, `(void*)0` | Ambiguous legacy macros | Built-in keyword: `nullptr` |
| `memset(&s, 0, sizeof(s))` | Legacy zeroing | Empty initializer: `s = {}` |
| `#define CONSTANT 10` | Untyped macro constants | Strongly-typed `constexpr`: `constexpr uint32_t CONSTANT = 10;` |

### 2.2 Standard C23 Attributes
Use standard attributes natively:
```c
[[nodiscard]]
khr_result_t khr_ring_submit(khr_ring_t* ring);

[[maybe_unused]]
static void debug_dump_sqe(const struct io_uring_sqe* sqe);
```

---

## 3. I/O Subsystem: Raw `io_uring`

We do **not** link or use `liburing`. All operations are executed through direct syscalls to the kernel:

1. **Ring Setup**: `syscall(__NR_io_uring_setup, depth, &params)`
   - Modern setup flags used: `IORING_SETUP_COOP_TASKRUN`, `IORING_SETUP_DEFER_TASKRUN`, `IORING_SETUP_SINGLE_ISSUER`.
   - Single MMAP validation: Check `params.features & IORING_FEAT_SINGLE_MMAP`.
2. **Ring Entry**: `syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags, sig)`
3. **Ring Registration**: `syscall(__NR_io_uring_register, fd, opcode, arg, nr_args)`
   - Direct ring registration: `IORING_REGISTER_RING_FDS`.
4. **Futex Synchronization**: Use `IORING_OP_FUTEX_WAIT` and `IORING_OP_FUTEX_WAKE` for low-latency job synchronization.

> **RULE**: `epoll` and `poll` are strictly forbidden across the entire repository.

---

## 4. Graphics & Compute Subsystem: Vulkan 1.4

1. **Version Baseline**: `VK_API_VERSION_1_4` required at instance and device creation.
2. **Windowing & Display**:
   - `libwayland` is strictly forbidden.
   - For hardware display: use Vulkan KMS/DRM Direct-to-Display (`VK_KHR_display` and `VK_EXT_direct_mode_display`).
   - For validation and compute: use `VK_EXT_headless_surface` or offscreen framebuffer/compute pipelines.
3. **Synchronization 2**: Exclusively use `vkQueueSubmit2` with `VkCommandBufferSubmitInfo` and `vkCmdPipelineBarrier2`.

---

## 5. Build & Verification Commands

```bash
# Build engine binary
make all

# Run engine
make run

# Clean build artifacts
make clean
```

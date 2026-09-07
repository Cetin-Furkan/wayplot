#ifndef KHOROS_GFX_BDA_ARENA_H
#define KHOROS_GFX_BDA_ARENA_H

#if __STDC_VERSION__ < 202311L
#error "Khoros requires pure ISO C23 (compile with -std=c23)"
#endif

#ifdef _STDBOOL_H
#error "<stdbool.h> is strictly banned. In C23, bool, true, and false are native language keywords."
#endif

#ifdef _STDALIGN_H
#error "<stdalign.h> is strictly banned. In C23, alignas and alignof are native language keywords."
#endif

#include <stdint.h>
#include <stddef.h>
#include <vulkan/vulkan.h>
#include "khoros/gfx/device.h"
#include "khoros/uring/ring.h"

/* Default arena size: 2 MiB matching Khoros hugepage arena */
constexpr size_t KHR_BDA_DEFAULT_ARENA_SZ = 2'097'152; /* 2 MiB */

typedef struct khr_bda_arena {
    const khr_gfx_device_t* dev;
    VkBuffer        buffer;
    VkDeviceMemory  memory;
    VkDeviceAddress gpu_address;
    void*           host_ptr;
    union {
        size_t      size;
        size_t      capacity;
    };
    size_t          head;
    bool            registered_with_uring;
    bool            is_imported;    /* true if host memory was imported via VK_EXT_external_memory_host */
    bool            owns_host_ptr;  /* true if host_ptr was mmap'd inside arena and must be munmap'd in destroy */
} khr_bda_arena_t;

/*
 * Allocate a host-visible, host-coherent UMA memory arena paired with 2 MiB hugepages.
 * Attempts VK_EXT_external_memory_host import first to enable io_uring buffer registration;
 * falls back to native Vulkan vkAllocateMemory + vkMapMemory if extension is absent.
 */
[[nodiscard]]
bool khr_bda_arena_init(khr_gfx_device_t* d, khr_bda_arena_t* arena, size_t size);

/*
 * Initialize a BDA arena by wrapping an existing pre-allocated host buffer (such as topo->hugepage).
 * Requires VK_EXT_external_memory_host support.
 */
[[nodiscard]]
bool khr_bda_arena_init_with_host_ptr(khr_gfx_device_t* d, khr_bda_arena_t* arena,
                                      void* host_ptr, size_t size);

/*
 * Teardown and free all Vulkan and host resources associated with the arena.
 */
void khr_bda_arena_destroy(khr_gfx_device_t* d, khr_bda_arena_t* arena);

/*
 * Bump allocator within the BDA arena.
 */
[[nodiscard]]
bool khr_bda_arena_alloc(khr_bda_arena_t* arena, size_t size, size_t align,
                         void** out_host_ptr, VkDeviceAddress* out_gpu_addr);

/*
 * Reset bump allocation head to 0.
 */
void khr_bda_arena_reset(khr_bda_arena_t* arena);

/*
 * Register the arena's host-visible memory with io_uring Ring B via IORING_REGISTER_BUFFERS.
 * Enables zero-copy 3-SQE hardlinked atomic ingestion (READ_FIXED).
 */
[[nodiscard]]
bool khr_bda_arena_register_ring(khr_bda_arena_t* arena, khr_uring_t* ring);

/*
 * Unregister the arena's host memory from io_uring Ring B.
 */
void khr_bda_arena_unregister_ring(khr_bda_arena_t* arena, khr_uring_t* ring);

/*
 * Helper: Resolve a 64-bit GPU device address at a given byte offset.
 */
[[nodiscard]]
static inline VkDeviceAddress khr_bda_arena_address_at(const khr_bda_arena_t* arena, size_t offset) {
    return (arena != nullptr) ? (arena->gpu_address + (VkDeviceAddress)offset) : 0;
}

/*
 * Helper: Resolve a host CPU pointer at a given byte offset.
 */
[[nodiscard]]
static inline void* khr_bda_arena_host_at(const khr_bda_arena_t* arena, size_t offset) {
    return (arena != nullptr && arena->host_ptr != nullptr) ?
           ((uint8_t*)arena->host_ptr + offset) : nullptr;
}

#endif /* KHOROS_GFX_BDA_ARENA_H */

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
constexpr size_t KHR_BDA_DEFAULT_ARENA_SZ   = 2'097'152;       /* 2 MiB */
constexpr size_t KHR_BDA_VIRTUAL_POOL_SZ    = 1'073'741'824;   /* 1 GiB virtual address space */
constexpr size_t KHR_BDA_HUGEPAGE_COMMIT_SZ = 2'097'152;       /* 2 MiB physical commit block */

/* Power-of-two Slab Bucket Sizes */
constexpr size_t KHR_SLAB_BUCKET_64B   = 64;
constexpr size_t KHR_SLAB_BUCKET_256B  = 256;
constexpr size_t KHR_SLAB_BUCKET_1K    = 1024;
constexpr size_t KHR_SLAB_BUCKET_4K    = 4096;
constexpr size_t KHR_SLAB_NUM_BUCKETS  = 4;

typedef struct khr_slab_node {
    struct khr_slab_node* next;
    VkDeviceAddress       gpu_addr;
} khr_slab_node_t;

typedef struct {
    size_t           item_size;
    khr_slab_node_t* free_list;
    size_t           allocated_count;
    size_t           free_count;
} khr_slab_bucket_t;

typedef struct {
    khr_slab_bucket_t buckets[KHR_SLAB_NUM_BUCKETS];
} khr_slab_cache_t;

/* Frame-Transient TLS Scratch (Double/Triple-Buffered Ring) */
typedef struct {
    size_t          per_buffer_size;
    uint32_t        num_buffers;
    uint32_t        current_index;
    size_t          heads[3];
    void*           host_ptrs[3];
    VkDeviceAddress gpu_addrs[3];
} khr_frame_scratch_t;

constexpr size_t KHR_BDA_MAX_COMMITTED_BLOCKS = 512; /* 512 * 2 MiB = 1 GiB */

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

    /* Virtual Page-Table Arena extensions */
    size_t          virtual_reserve_sz; /* Virtual address reservation */
    size_t          committed_sz;       /* Physically committed size in 2 MiB blocks */
    bool            is_virtual_pool;    /* true if arena uses multi-hugepage virtual pool */
    khr_slab_cache_t slab_cache;        /* Two-level free-list slab cache */

    /* Static Asset Region */
    size_t          static_head;
    bool            static_frozen;

    /* Committed 2 MiB Block descriptors */
    uint32_t        block_count;
    VkBuffer        blocks_buffer[KHR_BDA_MAX_COMMITTED_BLOCKS];
    VkDeviceMemory  blocks_memory[KHR_BDA_MAX_COMMITTED_BLOCKS];
    VkDeviceAddress blocks_gpu_addr[KHR_BDA_MAX_COMMITTED_BLOCKS];
} khr_bda_arena_t;

/*
 * Allocate a host-visible, host-coherent UMA memory arena paired with 2 MiB hugepages.
 * Attempts VK_EXT_external_memory_host import first to enable io_uring buffer registration;
 * falls back to native Vulkan vkAllocateMemory + vkMapMemory if extension is absent.
 */
[[nodiscard]]
bool khr_bda_arena_init(khr_gfx_device_t* d, khr_bda_arena_t* arena, size_t size);

/*
 * Initialize a multi-hugepage Virtual Page-Table Arena reserving virtual_sz bytes
 * via mmap(MAP_NORESERVE | MAP_ANONYMOUS, PROT_NONE) and committing initial_commit_sz bytes.
 */
[[nodiscard]]
bool khr_bda_arena_init_virtual(khr_gfx_device_t* d, khr_bda_arena_t* arena,
                                size_t virtual_sz, size_t initial_commit_sz);

/*
 * On-demand commit of additional physical 2 MiB hugepage blocks into the virtual pool.
 */
[[nodiscard]]
bool khr_bda_arena_commit_more(khr_bda_arena_t* arena, size_t additional_bytes);

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
 * Bump allocator within the BDA arena (with on-demand commit when virtual pool is enabled).
 */
[[nodiscard]]
bool khr_bda_arena_alloc(khr_bda_arena_t* arena, size_t size, size_t align,
                         void** out_host_ptr, VkDeviceAddress* out_gpu_addr);

/*
 * Bump allocator for the Static Asset Region (immutable assets: meshes, indices, pipeline tables).
 */
[[nodiscard]]
bool khr_bda_arena_alloc_static(khr_bda_arena_t* arena, size_t size, size_t align,
                                void** out_host_ptr, VkDeviceAddress* out_gpu_addr);

/*
 * Freeze the Static Asset Region, rendering it immutable once loaded.
 */
void khr_bda_arena_freeze_static(khr_bda_arena_t* arena);

/*
 * Reset bump allocation head to 0 (or to static_head if static region is frozen).
 */
void khr_bda_arena_reset(khr_bda_arena_t* arena);

/*
 * Slab cache lifecycle and allocation: fixed power-of-two buckets (64B, 256B, 1KiB, 4KiB).
 */
void khr_slab_cache_init(khr_slab_cache_t* cache);

[[nodiscard]]
bool khr_slab_alloc(khr_bda_arena_t* arena, khr_slab_cache_t* cache, size_t size,
                    void** out_host_ptr, VkDeviceAddress* out_gpu_addr);

void khr_slab_free(khr_slab_cache_t* cache, void* host_ptr, VkDeviceAddress gpu_addr, size_t size);

/*
 * Frame-Transient TLS Scratch lifecycle: double/triple-buffered ring buffers.
 */
[[nodiscard]]
bool khr_frame_scratch_init(khr_bda_arena_t* arena, khr_frame_scratch_t* scratch,
                            size_t per_frame_sz, uint32_t num_frames);

void khr_frame_scratch_reset(khr_frame_scratch_t* scratch, uint32_t frame_index);

[[nodiscard]]
bool khr_frame_scratch_alloc(khr_frame_scratch_t* scratch, size_t size, size_t align,
                             void** out_host_ptr, VkDeviceAddress* out_gpu_addr);

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
    if (arena == nullptr) return 0;
    if (arena->is_virtual_pool && arena->block_count > 0) {
        uint32_t b_idx = (uint32_t)(offset / KHR_BDA_HUGEPAGE_COMMIT_SZ);
        if (b_idx < arena->block_count && arena->blocks_gpu_addr[b_idx] != 0) {
            return arena->blocks_gpu_addr[b_idx] + (VkDeviceAddress)(offset % KHR_BDA_HUGEPAGE_COMMIT_SZ);
        }
    }
    return arena->gpu_address + (VkDeviceAddress)offset;
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

#include "khoros/gfx/bda_arena.h"
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>

#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25
#endif

#ifndef MAP_HUGE_2MB
#define MAP_HUGE_2MB (21 << MAP_HUGE_SHIFT)
#endif

[[nodiscard]]
static uint32_t khr_bda_find_memory_type(VkPhysicalDevice phy, uint32_t type_filter,
                                         VkMemoryPropertyFlags req_flags) {
    VkPhysicalDeviceMemoryProperties mem_props = {};
    vkGetPhysicalDeviceMemoryProperties(phy, &mem_props);

    /* Priority 1: Exact match including DEVICE_LOCAL for true UMA zero-copy */
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((type_filter & (1U << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & (req_flags | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) ==
            (req_flags | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            return i;
        }
    }
    /* Priority 2: Fallback to any memory type satisfying required flags */
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((type_filter & (1U << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & req_flags) == req_flags) {
            return i;
        }
    }
    return UINT32_MAX;
}

[[nodiscard]]
static void* khr_bda_alloc_hugepage_mem(size_t size) {
#ifndef __SANITIZE_ADDRESS__
    void* huge = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_2MB, -1, 0);
    if (huge != MAP_FAILED) {
        return huge;
    }
#endif
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (p == MAP_FAILED) {
        return nullptr;
    }
    (void)madvise(p, size, MADV_HUGEPAGE);
    (void)madvise(p, size, MADV_COLLAPSE);
    return p;
}

[[nodiscard]]
bool khr_bda_arena_init_with_host_ptr(khr_gfx_device_t* d, khr_bda_arena_t* arena,
                                      void* host_ptr, size_t size) {
    if (d == nullptr || d->device == VK_NULL_HANDLE || arena == nullptr ||
        host_ptr == nullptr || size == 0) {
        return false;
    }
    /* Address must meet page alignment requirements */
    if (((uintptr_t)host_ptr & 4095U) != 0) {
        return false;
    }
    *arena = (khr_bda_arena_t){};
    arena->dev = d;
    arena->size = size;

    /* Check for host pointer import extension */
    PFN_vkGetMemoryHostPointerPropertiesEXT pfn_host_props =
        d->vkGetMemoryHostPointerPropertiesEXT;
    if (pfn_host_props == nullptr) {
        pfn_host_props = (PFN_vkGetMemoryHostPointerPropertiesEXT)
            vkGetDeviceProcAddr(d->device, "vkGetMemoryHostPointerPropertiesEXT");
    }
    if (pfn_host_props == nullptr) {
        return false;
    }

    VkExternalMemoryBufferCreateInfo ext_bci = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
    };
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = &ext_bci,
        .size = size,
        .usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    VkBuffer buffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(d->device, &bci, nullptr, &buffer) != VK_SUCCESS) {
        return false;
    }

    VkMemoryHostPointerPropertiesEXT host_props = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT,
    };
    if (pfn_host_props(d->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                       host_ptr, &host_props) != VK_SUCCESS) {
        vkDestroyBuffer(d->device, buffer, nullptr);
        return false;
    }

    uint32_t mem_idx = khr_bda_find_memory_type(d->phy, host_props.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mem_idx == UINT32_MAX) {
        vkDestroyBuffer(d->device, buffer, nullptr);
        return false;
    }

    VkImportMemoryHostPointerInfoEXT import_info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
        .pHostPointer = host_ptr,
    };
    VkMemoryAllocateFlagsInfo flags_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .pNext = &import_info,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
    };
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &flags_info,
        .allocationSize = size,
        .memoryTypeIndex = mem_idx,
    };

    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(d->device, &ai, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyBuffer(d->device, buffer, nullptr);
        return false;
    }

    if (vkBindBufferMemory(d->device, buffer, memory, 0) != VK_SUCCESS) {
        vkFreeMemory(d->device, memory, nullptr);
        vkDestroyBuffer(d->device, buffer, nullptr);
        return false;
    }

    VkDeviceAddress addr = khr_gfx_get_buffer_address(d, buffer);
    if (addr == 0) {
        vkFreeMemory(d->device, memory, nullptr);
        vkDestroyBuffer(d->device, buffer, nullptr);
        return false;
    }

    arena->buffer = buffer;
    arena->memory = memory;
    arena->gpu_address = addr;
    arena->host_ptr = host_ptr;
    arena->is_imported = true;
    arena->owns_host_ptr = false;
    arena->registered_with_uring = false;
    return true;
}

[[nodiscard]]
bool khr_bda_arena_init(khr_gfx_device_t* d, khr_bda_arena_t* arena, size_t size) {
    if (d == nullptr || d->device == VK_NULL_HANDLE || arena == nullptr) {
        return false;
    }
    if (size == 0) {
        size = KHR_BDA_DEFAULT_ARENA_SZ;
    }
    /* Align size to 2 MiB */
    size = (size + (KHR_BDA_DEFAULT_ARENA_SZ - 1U)) & ~(KHR_BDA_DEFAULT_ARENA_SZ - 1U);
    *arena = (khr_bda_arena_t){};
    arena->dev = d;
    arena->size = size;

    /* Strategy 1: Attempt VK_EXT_external_memory_host import of hugepage mmap */
    if (d->has_external_memory_host || d->vkGetMemoryHostPointerPropertiesEXT != nullptr) {
        void* host_ptr = khr_bda_alloc_hugepage_mem(size);
        if (host_ptr != nullptr) {
            if (khr_bda_arena_init_with_host_ptr(d, arena, host_ptr, size)) {
                arena->owns_host_ptr = true;
                return true;
            }
            /* Import not supported by device, release mmap and try Strategy 2 */
            (void)munmap(host_ptr, size);
        }
    }

    /* Strategy 2: Native Vulkan allocation with vkMapMemory */
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    VkBuffer buffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(d->device, &bci, nullptr, &buffer) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements mem_reqs = {};
    vkGetBufferMemoryRequirements(d->device, buffer, &mem_reqs);

    uint32_t mem_idx = khr_bda_find_memory_type(d->phy, mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mem_idx == UINT32_MAX) {
        vkDestroyBuffer(d->device, buffer, nullptr);
        return false;
    }

    VkMemoryAllocateFlagsInfo flags_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
    };
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &flags_info,
        .allocationSize = mem_reqs.size,
        .memoryTypeIndex = mem_idx,
    };

    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(d->device, &ai, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyBuffer(d->device, buffer, nullptr);
        return false;
    }

    if (vkBindBufferMemory(d->device, buffer, memory, 0) != VK_SUCCESS) {
        vkFreeMemory(d->device, memory, nullptr);
        vkDestroyBuffer(d->device, buffer, nullptr);
        return false;
    }

    void* mapped = nullptr;
    if (vkMapMemory(d->device, memory, 0, size, 0, &mapped) != VK_SUCCESS) {
        vkFreeMemory(d->device, memory, nullptr);
        vkDestroyBuffer(d->device, buffer, nullptr);
        return false;
    }
    (void)madvise(mapped, size, MADV_HUGEPAGE);

    VkDeviceAddress addr = khr_gfx_get_buffer_address(d, buffer);
    if (addr == 0) {
        vkUnmapMemory(d->device, memory);
        vkFreeMemory(d->device, memory, nullptr);
        vkDestroyBuffer(d->device, buffer, nullptr);
        return false;
    }

    arena->buffer = buffer;
    arena->memory = memory;
    arena->gpu_address = addr;
    arena->host_ptr = mapped;
    arena->is_imported = false;
    arena->owns_host_ptr = false;
    arena->registered_with_uring = false;
    return true;
}

[[nodiscard]]
bool khr_bda_arena_init_virtual(khr_gfx_device_t* d, khr_bda_arena_t* arena,
                                size_t virtual_sz, size_t initial_commit_sz) {
    if (d == nullptr || d->device == VK_NULL_HANDLE || arena == nullptr) {
        return false;
    }
    if (virtual_sz == 0) {
        virtual_sz = KHR_BDA_VIRTUAL_POOL_SZ; /* 1 GiB */
    }
    if (initial_commit_sz == 0) {
        initial_commit_sz = KHR_BDA_HUGEPAGE_COMMIT_SZ; /* 2 MiB */
    }
    /* Align virtual_sz and initial_commit_sz to 2 MiB */
    virtual_sz = (virtual_sz + (KHR_BDA_HUGEPAGE_COMMIT_SZ - 1U)) & ~(KHR_BDA_HUGEPAGE_COMMIT_SZ - 1U);
    initial_commit_sz = (initial_commit_sz + (KHR_BDA_HUGEPAGE_COMMIT_SZ - 1U)) & ~(KHR_BDA_HUGEPAGE_COMMIT_SZ - 1U);
    if (initial_commit_sz > virtual_sz) {
        initial_commit_sz = virtual_sz;
    }

    *arena = (khr_bda_arena_t){};
    arena->dev = d;
    arena->virtual_reserve_sz = virtual_sz;
    arena->is_virtual_pool = true;
    khr_slab_cache_init(&arena->slab_cache);

    /* 1. Reserve virtual address space with PROT_NONE and MAP_NORESERVE */
    void* host_ptr = mmap(nullptr, virtual_sz, PROT_NONE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (host_ptr == MAP_FAILED) {
        return false;
    }

    /* 2. On-demand 2 MiB initial commit */
    if (mprotect(host_ptr, initial_commit_sz, PROT_READ | PROT_WRITE) != 0) {
        (void)munmap(host_ptr, virtual_sz);
        return false;
    }
    (void)madvise(host_ptr, initial_commit_sz, MADV_HUGEPAGE);
    (void)madvise(host_ptr, initial_commit_sz, MADV_COLLAPSE);

    arena->committed_sz = initial_commit_sz;
    arena->size = initial_commit_sz;

    /* 3. Register with Vulkan via host pointer import */
    if (d->has_external_memory_host || d->vkGetMemoryHostPointerPropertiesEXT != nullptr) {
        if (khr_bda_arena_init_with_host_ptr(d, arena, host_ptr, initial_commit_sz)) {
            arena->owns_host_ptr = true;
            arena->virtual_reserve_sz = virtual_sz;
            arena->committed_sz = initial_commit_sz;
            arena->is_virtual_pool = true;
            arena->block_count = 1;
            arena->blocks_buffer[0] = arena->buffer;
            arena->blocks_memory[0] = arena->memory;
            arena->blocks_gpu_addr[0] = arena->gpu_address;
            return true;
        }
    }

    /* Fallback: If external memory host import fails, clean up mmap and use standard init */
    (void)munmap(host_ptr, virtual_sz);
    if (khr_bda_arena_init(d, arena, initial_commit_sz)) {
        arena->virtual_reserve_sz = virtual_sz;
        arena->committed_sz = arena->size;
        arena->is_virtual_pool = true;
        arena->block_count = 1;
        arena->blocks_buffer[0] = arena->buffer;
        arena->blocks_memory[0] = arena->memory;
        arena->blocks_gpu_addr[0] = arena->gpu_address;
        return true;
    }
    return false;
}

[[nodiscard]]
bool khr_bda_arena_commit_more(khr_bda_arena_t* arena, size_t additional_bytes) {
    if (arena == nullptr || !arena->is_virtual_pool || arena->host_ptr == nullptr || !arena->is_imported) {
        return false;
    }
    if (additional_bytes == 0) {
        return true;
    }
    /* Align additional bytes to 2 MiB blocks */
    size_t commit_add = (additional_bytes + (KHR_BDA_HUGEPAGE_COMMIT_SZ - 1U)) & ~(KHR_BDA_HUGEPAGE_COMMIT_SZ - 1U);
    if (arena->committed_sz + commit_add > arena->virtual_reserve_sz) {
        return false;
    }

    size_t num_blocks_to_add = commit_add / KHR_BDA_HUGEPAGE_COMMIT_SZ;
    for (size_t b = 0; b < num_blocks_to_add; b++) {
        size_t block_offset = arena->committed_sz + (b * KHR_BDA_HUGEPAGE_COMMIT_SZ);
        void* target_addr = (uint8_t*)arena->host_ptr + block_offset;
        if (mprotect(target_addr, KHR_BDA_HUGEPAGE_COMMIT_SZ, PROT_READ | PROT_WRITE) != 0) {
            return false;
        }
        (void)madvise(target_addr, KHR_BDA_HUGEPAGE_COMMIT_SZ, MADV_HUGEPAGE);
        (void)madvise(target_addr, KHR_BDA_HUGEPAGE_COMMIT_SZ, MADV_COLLAPSE);

        if (arena->is_imported && arena->dev != nullptr && arena->block_count < KHR_BDA_MAX_COMMITTED_BLOCKS) {
            const khr_gfx_device_t* d = arena->dev;
            PFN_vkGetMemoryHostPointerPropertiesEXT pfn_host_props = d->vkGetMemoryHostPointerPropertiesEXT;
            if (pfn_host_props != nullptr) {
                VkExternalMemoryBufferCreateInfo ext_bci = {
                    .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                    .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                };
                VkBufferCreateInfo bci = {
                    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                    .pNext = &ext_bci,
                    .size = KHR_BDA_HUGEPAGE_COMMIT_SZ,
                    .usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                             VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                };
                VkBuffer buf = VK_NULL_HANDLE;
                if (vkCreateBuffer(d->device, &bci, nullptr, &buf) == VK_SUCCESS) {
                    VkMemoryHostPointerPropertiesEXT host_props = {
                        .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT,
                    };
                    if (pfn_host_props(d->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                       target_addr, &host_props) == VK_SUCCESS) {
                        uint32_t mem_idx = khr_bda_find_memory_type(d->phy, host_props.memoryTypeBits,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                        if (mem_idx != UINT32_MAX) {
                            VkImportMemoryHostPointerInfoEXT import_info = {
                                .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                .pHostPointer = target_addr,
                            };
                            VkMemoryAllocateFlagsInfo flags_info = {
                                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                .pNext = &import_info,
                                .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
                            };
                            VkMemoryAllocateInfo ai = {
                                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                .pNext = &flags_info,
                                .allocationSize = KHR_BDA_HUGEPAGE_COMMIT_SZ,
                                .memoryTypeIndex = mem_idx,
                            };
                            VkDeviceMemory mem = VK_NULL_HANDLE;
                            if (vkAllocateMemory(d->device, &ai, nullptr, &mem) == VK_SUCCESS) {
                                if (vkBindBufferMemory(d->device, buf, mem, 0) == VK_SUCCESS) {
                                    VkDeviceAddress addr = khr_gfx_get_buffer_address(d, buf);
                                    uint32_t b_idx = arena->block_count++;
                                    arena->blocks_buffer[b_idx] = buf;
                                    arena->blocks_memory[b_idx] = mem;
                                    arena->blocks_gpu_addr[b_idx] = addr;
                                    buf = VK_NULL_HANDLE;
                                    mem = VK_NULL_HANDLE;
                                }
                                if (mem != VK_NULL_HANDLE) vkFreeMemory(d->device, mem, nullptr);
                            }
                        }
                    }
                    if (buf != VK_NULL_HANDLE) vkDestroyBuffer(d->device, buf, nullptr);
                }
            }
        }
    }

    arena->committed_sz += commit_add;
    arena->size = arena->committed_sz;
    return true;
}

void khr_bda_arena_destroy(khr_gfx_device_t* d, khr_bda_arena_t* arena) {
    if (d == nullptr || arena == nullptr) {
        return;
    }
    if (!arena->is_imported && arena->host_ptr != nullptr && arena->memory != VK_NULL_HANDLE) {
        vkUnmapMemory(d->device, arena->memory);
    }
    if (arena->buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(d->device, arena->buffer, nullptr);
    }
    if (arena->memory != VK_NULL_HANDLE) {
        vkFreeMemory(d->device, arena->memory, nullptr);
    }
    for (uint32_t i = 1; i < arena->block_count; i++) {
        if (arena->blocks_buffer[i] != VK_NULL_HANDLE) {
            vkDestroyBuffer(d->device, arena->blocks_buffer[i], nullptr);
        }
        if (arena->blocks_memory[i] != VK_NULL_HANDLE) {
            vkFreeMemory(d->device, arena->blocks_memory[i], nullptr);
        }
    }
    if (arena->is_virtual_pool && arena->owns_host_ptr && arena->host_ptr != nullptr) {
        (void)munmap(arena->host_ptr, arena->virtual_reserve_sz);
    } else if (arena->is_imported && arena->owns_host_ptr && arena->host_ptr != nullptr) {
        (void)munmap(arena->host_ptr, arena->size);
    }
    *arena = (khr_bda_arena_t){};
}

[[nodiscard]]
bool khr_bda_arena_alloc(khr_bda_arena_t* arena, size_t size, size_t align,
                         void** out_host_ptr, VkDeviceAddress* out_gpu_addr) {
    if (!arena || !arena->host_ptr || size == 0) return false;
    if (align == 0) align = 8;
    if ((align & (align - 1U)) != 0) {
        return false;
    }
    size_t aligned_head = (arena->head + (align - 1U)) & ~(align - 1U);
    if (arena->is_virtual_pool && (aligned_head + size > arena->committed_sz)) {
        size_t needed = (aligned_head + size) - arena->committed_sz;
        if (!khr_bda_arena_commit_more(arena, needed)) {
            return false;
        }
    } else if (aligned_head + size > arena->size) {
        return false;
    }
    if (out_host_ptr) {
        *out_host_ptr = (uint8_t*)arena->host_ptr + aligned_head;
    }
    if (out_gpu_addr) {
        *out_gpu_addr = khr_bda_arena_address_at(arena, aligned_head);
    }
    arena->head = aligned_head + size;
    return true;
}

[[nodiscard]]
bool khr_bda_arena_alloc_static(khr_bda_arena_t* arena, size_t size, size_t align,
                                void** out_host_ptr, VkDeviceAddress* out_gpu_addr) {
    if (arena == nullptr || arena->static_frozen) {
        return false;
    }
    bool ok = khr_bda_arena_alloc(arena, size, align, out_host_ptr, out_gpu_addr);
    if (ok) {
        arena->static_head = arena->head;
    }
    return ok;
}

void khr_bda_arena_freeze_static(khr_bda_arena_t* arena) {
    if (arena == nullptr) return;
    arena->static_frozen = true;
    arena->static_head = arena->head;
}

void khr_bda_arena_reset(khr_bda_arena_t* arena) {
    if (arena) {
        arena->head = arena->static_frozen ? arena->static_head : 0;
    }
}

void khr_slab_cache_init(khr_slab_cache_t* cache) {
    if (cache == nullptr) return;
    *cache = (khr_slab_cache_t){};
    cache->buckets[0].item_size = KHR_SLAB_BUCKET_64B;
    cache->buckets[1].item_size = KHR_SLAB_BUCKET_256B;
    cache->buckets[2].item_size = KHR_SLAB_BUCKET_1K;
    cache->buckets[3].item_size = KHR_SLAB_BUCKET_4K;
}

static inline int khr_slab_get_bucket_index(size_t size) {
    if (size <= KHR_SLAB_BUCKET_64B) return 0;
    if (size <= KHR_SLAB_BUCKET_256B) return 1;
    if (size <= KHR_SLAB_BUCKET_1K) return 2;
    if (size <= KHR_SLAB_BUCKET_4K) return 3;
    return -1;
}

[[nodiscard]]
bool khr_slab_alloc(khr_bda_arena_t* arena, khr_slab_cache_t* cache, size_t size,
                    void** out_host_ptr, VkDeviceAddress* out_gpu_addr) {
    if (arena == nullptr || cache == nullptr || size == 0 || out_host_ptr == nullptr) {
        return false;
    }
    int b = khr_slab_get_bucket_index(size);
    if (b < 0) {
        /* Exceeds 4 KiB bucket size, fall back to arena bump allocation */
        return khr_bda_arena_alloc(arena, size, 64, out_host_ptr, out_gpu_addr);
    }
    khr_slab_bucket_t* bucket = &cache->buckets[b];
    if (bucket->free_list != nullptr) {
        khr_slab_node_t* node = bucket->free_list;
        bucket->free_list = node->next;
        *out_host_ptr = (void*)node;
        if (out_gpu_addr) {
            *out_gpu_addr = node->gpu_addr;
        }
        bucket->free_count--;
        bucket->allocated_count++;
        return true;
    }

    /* Free list is empty: carve a 64 KiB slab from arena */
    constexpr size_t SLAB_BLOCK_SZ = 64 * 1024;
    size_t item_sz = bucket->item_size;
    void* slab_host = nullptr;
    VkDeviceAddress slab_gpu = 0;
    if (!khr_bda_arena_alloc(arena, SLAB_BLOCK_SZ, item_sz, &slab_host, &slab_gpu)) {
        /* Try carving single item */
        return khr_bda_arena_alloc(arena, item_sz, item_sz, out_host_ptr, out_gpu_addr);
    }

    uint32_t num_items = (uint32_t)(SLAB_BLOCK_SZ / item_sz);
    *out_host_ptr = slab_host;
    if (out_gpu_addr) {
        *out_gpu_addr = slab_gpu;
    }
    bucket->allocated_count++;

    /* Link remaining items into free list */
    for (uint32_t i = 1; i < num_items; i++) {
        void* item_host = (uint8_t*)slab_host + (i * item_sz);
        VkDeviceAddress item_gpu = slab_gpu + (VkDeviceAddress)(i * item_sz);
        khr_slab_node_t* node = (khr_slab_node_t*)item_host;
        node->next = bucket->free_list;
        node->gpu_addr = item_gpu;
        bucket->free_list = node;
        bucket->free_count++;
    }
    return true;
}

void khr_slab_free(khr_slab_cache_t* cache, void* host_ptr, VkDeviceAddress gpu_addr, size_t size) {
    if (cache == nullptr || host_ptr == nullptr || size == 0) {
        return;
    }
    int b = khr_slab_get_bucket_index(size);
    if (b < 0) {
        return; /* Large fallback allocations are reclaimed on arena reset */
    }
    khr_slab_bucket_t* bucket = &cache->buckets[b];
    khr_slab_node_t* node = (khr_slab_node_t*)host_ptr;
    node->gpu_addr = gpu_addr;
    node->next = bucket->free_list;
    bucket->free_list = node;
    bucket->free_count++;
    if (bucket->allocated_count > 0) {
        bucket->allocated_count--;
    }
}

[[nodiscard]]
bool khr_frame_scratch_init(khr_bda_arena_t* arena, khr_frame_scratch_t* scratch,
                            size_t per_frame_sz, uint32_t num_frames) {
    if (arena == nullptr || scratch == nullptr || per_frame_sz == 0) {
        return false;
    }
    if (num_frames < 2) num_frames = 2;
    if (num_frames > 3) num_frames = 3;

    *scratch = (khr_frame_scratch_t){
        .per_buffer_size = per_frame_sz,
        .num_buffers = num_frames,
        .current_index = 0,
    };

    for (uint32_t i = 0; i < num_frames; i++) {
        if (!khr_bda_arena_alloc(arena, per_frame_sz, 64,
                                 &scratch->host_ptrs[i], &scratch->gpu_addrs[i])) {
            return false;
        }
        scratch->heads[i] = 0;
    }
    return true;
}

void khr_frame_scratch_reset(khr_frame_scratch_t* scratch, uint32_t frame_index) {
    if (scratch == nullptr || scratch->num_buffers == 0) return;
    scratch->current_index = frame_index % scratch->num_buffers;
    scratch->heads[scratch->current_index] = 0;
}

[[nodiscard]]
bool khr_frame_scratch_alloc(khr_frame_scratch_t* scratch, size_t size, size_t align,
                             void** out_host_ptr, VkDeviceAddress* out_gpu_addr) {
    if (scratch == nullptr || size == 0 || scratch->num_buffers == 0) return false;
    if (align == 0) align = 8;
    if ((align & (align - 1U)) != 0) return false;

    uint32_t cur = scratch->current_index;
    size_t aligned_head = (scratch->heads[cur] + (align - 1U)) & ~(align - 1U);
    if (aligned_head + size > scratch->per_buffer_size) {
        return false;
    }
    if (out_host_ptr) {
        *out_host_ptr = (uint8_t*)scratch->host_ptrs[cur] + aligned_head;
    }
    if (out_gpu_addr) {
        *out_gpu_addr = scratch->gpu_addrs[cur] + (VkDeviceAddress)aligned_head;
    }
    scratch->heads[cur] = aligned_head + size;
    return true;
}

[[nodiscard]]
bool khr_bda_arena_register_ring(khr_bda_arena_t* arena, khr_uring_t* ring) {
    if (arena == nullptr || ring == nullptr || arena->host_ptr == nullptr || arena->size == 0) {
        return false;
    }
    if (arena->registered_with_uring) {
        return true;
    }
    struct iovec iov = {
        .iov_base = arena->host_ptr,
        .iov_len = arena->size,
    };
    if (!khr_uring_register_buffers(ring, &iov, 1)) {
        return false;
    }
    arena->registered_with_uring = true;
    return true;
}

void khr_bda_arena_unregister_ring(khr_bda_arena_t* arena, khr_uring_t* ring) {
    if (arena == nullptr || ring == nullptr) {
        return;
    }
    if (arena->registered_with_uring) {
        (void)khr_uring_unregister_buffers(ring);
        arena->registered_with_uring = false;
    }
}

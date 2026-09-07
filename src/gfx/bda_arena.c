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
        .usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
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
        .usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
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
    if (arena->is_imported && arena->owns_host_ptr && arena->host_ptr != nullptr) {
        (void)munmap(arena->host_ptr, arena->size);
    }
    *arena = (khr_bda_arena_t){};
}

[[nodiscard]]
bool khr_bda_arena_alloc(khr_bda_arena_t* arena, size_t size, size_t align,
                         void** out_host_ptr, VkDeviceAddress* out_gpu_addr) {
    if (!arena || !arena->host_ptr || size == 0) return false;
    if (align == 0) align = 8;
    size_t aligned_head = (arena->head + (align - 1U)) & ~(align - 1U);
    if (aligned_head + size > arena->size) {
        return false;
    }
    if (out_host_ptr) {
        *out_host_ptr = (uint8_t*)arena->host_ptr + aligned_head;
    }
    if (out_gpu_addr) {
        *out_gpu_addr = arena->gpu_address + (VkDeviceAddress)aligned_head;
    }
    arena->head = aligned_head + size;
    return true;
}

void khr_bda_arena_reset(khr_bda_arena_t* arena) {
    if (arena) {
        arena->head = 0;
    }
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

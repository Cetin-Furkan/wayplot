#ifndef KHOROS_GFX_DESCRIPTOR_BUFFER_H
#define KHOROS_GFX_DESCRIPTOR_BUFFER_H

#if __STDC_VERSION__ < 202311L
#error "Khoros requires pure ISO C23 (compile with -std=c23)"
#endif

#ifdef _STDBOOL_H
#error "<stdbool.h> is strictly banned. In C23, bool, true, and false are native language keywords."
#endif

#ifdef _STDALIGN_H
#error "<stdalign.h> is strictly banned. In C23, alignas and alignof are native language keywords."
#endif

#include "khoros/core/attributes.h"
#include "khoros/gfx/device.h"
#include <stdint.h>
#include <vulkan/vulkan.h>

constexpr uint32_t KHR_DESCRIPTOR_HEAP_DEFAULT_MAX_TEXTURES = 1024;
constexpr uint32_t KHR_DESCRIPTOR_HEAP_DEFAULT_MAX_SAMPLERS = 64;

/*
 * Unified Descriptor Heap using VK_EXT_descriptor_buffer (Pillar 4).
 * Eliminates VkDescriptorPool, VkDescriptorSet, and vkUpdateDescriptorSets.
 * Stores sampler and sampled-image descriptors directly in host-coherent GPU memory.
 */
typedef struct {
    const khr_gfx_device_t* dev;

    /* Resource Descriptor Buffer (Sampled Images / Textures) */
    VkBuffer        resource_buffer;
    VkDeviceMemory  resource_memory;
    void*           resource_host_map;
    VkDeviceAddress resource_bda;
    size_t          resource_total_bytes;
    uint32_t        resource_descriptor_size;
    uint32_t        max_textures;
    uint32_t        texture_count;

    /* Sampler Descriptor Buffer */
    VkBuffer        sampler_buffer;
    VkDeviceMemory  sampler_memory;
    void*           sampler_host_map;
    VkDeviceAddress sampler_bda;
    size_t          sampler_total_bytes;
    uint32_t        sampler_descriptor_size;
    uint32_t        max_samplers;
    uint32_t        sampler_count;

    /* Descriptor Set Layouts created with VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT */
    VkDescriptorSetLayout resource_set_layout;
    VkDeviceSize          resource_layout_size;
    VkDeviceSize          resource_binding_offset;

    VkDescriptorSetLayout sampler_set_layout;
    VkDeviceSize          sampler_layout_size;
    VkDeviceSize          sampler_binding_offset;

    bool initialized;
} khr_descriptor_heap_t;

/*
 * Initialize the unified descriptor heap on the Vulkan device.
 * Allocates host-coherent descriptor memory with VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT
 * and VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT.
 */
[[nodiscard]]
bool khr_descriptor_heap_init(khr_descriptor_heap_t* heap,
                              const khr_gfx_device_t* dev,
                              uint32_t max_textures,
                              uint32_t max_samplers);

/*
 * Teardown and release descriptor buffers, memory, and set layouts.
 */
void khr_descriptor_heap_destroy(khr_descriptor_heap_t* heap);

/*
 * Direct-memory registration: writes sampled image descriptor directly into host-mapped
 * descriptor buffer memory using vkGetDescriptorEXT.
 * Returns texture index in the heap, or UINT32_MAX on capacity overflow.
 */
uint32_t khr_descriptor_heap_register_texture(khr_descriptor_heap_t* heap,
                                              VkImageView view,
                                              VkImageLayout layout);

/*
 * Direct-memory registration: writes sampler descriptor directly into host-mapped
 * descriptor buffer memory using vkGetDescriptorEXT.
 * Returns sampler index in the heap, or UINT32_MAX on capacity overflow.
 */
uint32_t khr_descriptor_heap_register_sampler(khr_descriptor_heap_t* heap,
                                              VkSampler sampler);

/*
 * Bind descriptor buffers to command buffer.
 * Replaces vkCmdBindDescriptorSets with zero CPU overhead.
 */
void khr_descriptor_heap_bind(const khr_descriptor_heap_t* heap,
                              VkCommandBuffer cmd,
                              VkPipelineBindPoint bind_point,
                              VkPipelineLayout pipeline_layout);

/*
 * GPU device address accessors for direct BDA addressing in push constants / shaders.
 */
[[nodiscard]]
VkDeviceAddress khr_descriptor_heap_get_resource_bda(const khr_descriptor_heap_t* heap);

[[nodiscard]]
VkDeviceAddress khr_descriptor_heap_get_sampler_bda(const khr_descriptor_heap_t* heap);

#endif /* KHOROS_GFX_DESCRIPTOR_BUFFER_H */

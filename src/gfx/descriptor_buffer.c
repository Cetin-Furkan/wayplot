#include "khoros/gfx/descriptor_buffer.h"
#include <string.h>

[[nodiscard]]
static uint32_t khr_desc_find_memory_type(VkPhysicalDevice phy, uint32_t type_filter,
                                          VkMemoryPropertyFlags req_flags) {
    VkPhysicalDeviceMemoryProperties mem_props = {};
    vkGetPhysicalDeviceMemoryProperties(phy, &mem_props);

    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((type_filter & (1U << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & (req_flags | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) ==
            (req_flags | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            return i;
        }
    }
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((type_filter & (1U << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & req_flags) == req_flags) {
            return i;
        }
    }
    return UINT32_MAX;
}

static inline size_t khr_align_up(size_t size, size_t alignment) {
    if (alignment == 0) return size;
    return (size + alignment - 1) & ~(alignment - 1);
}

[[nodiscard]]
bool khr_descriptor_heap_init(khr_descriptor_heap_t* heap,
                              const khr_gfx_device_t* dev,
                              uint32_t max_textures,
                              uint32_t max_samplers) {
    if (heap == nullptr || dev == nullptr || dev->device == VK_NULL_HANDLE) {
        return false;
    }
    if (!dev->has_descriptor_buffer || dev->vkGetDescriptorEXT == nullptr) {
        return false;
    }

    memset(heap, 0, sizeof(*heap));
    heap->dev = dev;
    heap->max_textures = (max_textures > 0) ? max_textures : KHR_DESCRIPTOR_HEAP_DEFAULT_MAX_TEXTURES;
    heap->max_samplers = (max_samplers > 0) ? max_samplers : KHR_DESCRIPTOR_HEAP_DEFAULT_MAX_SAMPLERS;

    heap->resource_descriptor_size = (uint32_t)dev->descriptor_buffer_props.sampledImageDescriptorSize;
    heap->sampler_descriptor_size = (uint32_t)dev->descriptor_buffer_props.samplerDescriptorSize;
    size_t align = (size_t)dev->descriptor_buffer_props.descriptorBufferOffsetAlignment;
    if (align == 0) align = 64;

    /* 1. Create Resource (Sampled Image) Descriptor Set Layout */
    VkDescriptorSetLayoutBinding res_binding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        .descriptorCount = heap->max_textures,
        .stageFlags = VK_SHADER_STAGE_ALL,
    };
    VkDescriptorSetLayoutCreateInfo res_layout_ci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
        .bindingCount = 1,
        .pBindings = &res_binding,
    };
    if (vkCreateDescriptorSetLayout(dev->device, &res_layout_ci, nullptr,
                                    &heap->resource_set_layout) != VK_SUCCESS) {
        khr_descriptor_heap_destroy(heap);
        return false;
    }
    dev->vkGetDescriptorSetLayoutSizeEXT(dev->device, heap->resource_set_layout,
                                         &heap->resource_layout_size);
    dev->vkGetDescriptorSetLayoutBindingOffsetEXT(dev->device, heap->resource_set_layout,
                                                  0, &heap->resource_binding_offset);

    /* 2. Create Sampler Descriptor Set Layout */
    VkDescriptorSetLayoutBinding samp_binding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
        .descriptorCount = heap->max_samplers,
        .stageFlags = VK_SHADER_STAGE_ALL,
    };
    VkDescriptorSetLayoutCreateInfo samp_layout_ci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
        .bindingCount = 1,
        .pBindings = &samp_binding,
    };
    if (vkCreateDescriptorSetLayout(dev->device, &samp_layout_ci, nullptr,
                                    &heap->sampler_set_layout) != VK_SUCCESS) {
        khr_descriptor_heap_destroy(heap);
        return false;
    }
    dev->vkGetDescriptorSetLayoutSizeEXT(dev->device, heap->sampler_set_layout,
                                         &heap->sampler_layout_size);
    dev->vkGetDescriptorSetLayoutBindingOffsetEXT(dev->device, heap->sampler_set_layout,
                                                  0, &heap->sampler_binding_offset);

    /* 3. Allocate Resource Descriptor Buffer */
    size_t req_res_bytes = (heap->resource_layout_size > 0)
        ? (size_t)heap->resource_layout_size
        : (size_t)heap->max_textures * heap->resource_descriptor_size;
    heap->resource_total_bytes = khr_align_up(req_res_bytes, align);

    VkBufferCreateInfo res_bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = heap->resource_total_bytes,
        .usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateBuffer(dev->device, &res_bci, nullptr, &heap->resource_buffer) != VK_SUCCESS) {
        khr_descriptor_heap_destroy(heap);
        return false;
    }

    VkMemoryRequirements res_mem_reqs = {};
    vkGetBufferMemoryRequirements(dev->device, heap->resource_buffer, &res_mem_reqs);
    uint32_t res_mem_type = khr_desc_find_memory_type(dev->phy, res_mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (res_mem_type == UINT32_MAX) {
        khr_descriptor_heap_destroy(heap);
        return false;
    }

    VkMemoryAllocateFlagsInfo flags_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
    };
    VkMemoryAllocateInfo res_ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &flags_info,
        .allocationSize = res_mem_reqs.size,
        .memoryTypeIndex = res_mem_type,
    };
    if (vkAllocateMemory(dev->device, &res_ai, nullptr, &heap->resource_memory) != VK_SUCCESS) {
        khr_descriptor_heap_destroy(heap);
        return false;
    }
    if (vkBindBufferMemory(dev->device, heap->resource_buffer, heap->resource_memory, 0) != VK_SUCCESS) {
        khr_descriptor_heap_destroy(heap);
        return false;
    }
    if (vkMapMemory(dev->device, heap->resource_memory, 0, heap->resource_total_bytes, 0,
                    &heap->resource_host_map) != VK_SUCCESS) {
        khr_descriptor_heap_destroy(heap);
        return false;
    }
    memset(heap->resource_host_map, 0, heap->resource_total_bytes);

    VkBufferDeviceAddressInfo res_addr_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = heap->resource_buffer,
    };
    heap->resource_bda = dev->vkGetBufferDeviceAddress(dev->device, &res_addr_info);

    /* 4. Allocate Sampler Descriptor Buffer */
    size_t req_samp_bytes = (heap->sampler_layout_size > 0)
        ? (size_t)heap->sampler_layout_size
        : (size_t)heap->max_samplers * heap->sampler_descriptor_size;
    heap->sampler_total_bytes = khr_align_up(req_samp_bytes, align);

    VkBufferCreateInfo samp_bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = heap->sampler_total_bytes,
        .usage = VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateBuffer(dev->device, &samp_bci, nullptr, &heap->sampler_buffer) != VK_SUCCESS) {
        khr_descriptor_heap_destroy(heap);
        return false;
    }

    VkMemoryRequirements samp_mem_reqs = {};
    vkGetBufferMemoryRequirements(dev->device, heap->sampler_buffer, &samp_mem_reqs);
    uint32_t samp_mem_type = khr_desc_find_memory_type(dev->phy, samp_mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (samp_mem_type == UINT32_MAX) {
        khr_descriptor_heap_destroy(heap);
        return false;
    }

    VkMemoryAllocateInfo samp_ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &flags_info,
        .allocationSize = samp_mem_reqs.size,
        .memoryTypeIndex = samp_mem_type,
    };
    if (vkAllocateMemory(dev->device, &samp_ai, nullptr, &heap->sampler_memory) != VK_SUCCESS) {
        khr_descriptor_heap_destroy(heap);
        return false;
    }
    if (vkBindBufferMemory(dev->device, heap->sampler_buffer, heap->sampler_memory, 0) != VK_SUCCESS) {
        khr_descriptor_heap_destroy(heap);
        return false;
    }
    if (vkMapMemory(dev->device, heap->sampler_memory, 0, heap->sampler_total_bytes, 0,
                    &heap->sampler_host_map) != VK_SUCCESS) {
        khr_descriptor_heap_destroy(heap);
        return false;
    }
    memset(heap->sampler_host_map, 0, heap->sampler_total_bytes);

    VkBufferDeviceAddressInfo samp_addr_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = heap->sampler_buffer,
    };
    heap->sampler_bda = dev->vkGetBufferDeviceAddress(dev->device, &samp_addr_info);

    heap->initialized = true;
    return true;
}

void khr_descriptor_heap_destroy(khr_descriptor_heap_t* heap) {
    if (heap == nullptr || heap->dev == nullptr) {
        return;
    }
    VkDevice dev = heap->dev->device;
    if (dev == VK_NULL_HANDLE) {
        return;
    }

    if (heap->resource_host_map != nullptr) {
        vkUnmapMemory(dev, heap->resource_memory);
        heap->resource_host_map = nullptr;
    }
    if (heap->resource_buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(dev, heap->resource_buffer, nullptr);
        heap->resource_buffer = VK_NULL_HANDLE;
    }
    if (heap->resource_memory != VK_NULL_HANDLE) {
        vkFreeMemory(dev, heap->resource_memory, nullptr);
        heap->resource_memory = VK_NULL_HANDLE;
    }
    if (heap->resource_set_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, heap->resource_set_layout, nullptr);
        heap->resource_set_layout = VK_NULL_HANDLE;
    }

    if (heap->sampler_host_map != nullptr) {
        vkUnmapMemory(dev, heap->sampler_memory);
        heap->sampler_host_map = nullptr;
    }
    if (heap->sampler_buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(dev, heap->sampler_buffer, nullptr);
        heap->sampler_buffer = VK_NULL_HANDLE;
    }
    if (heap->sampler_memory != VK_NULL_HANDLE) {
        vkFreeMemory(dev, heap->sampler_memory, nullptr);
        heap->sampler_memory = VK_NULL_HANDLE;
    }
    if (heap->sampler_set_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, heap->sampler_set_layout, nullptr);
        heap->sampler_set_layout = VK_NULL_HANDLE;
    }

    heap->initialized = false;
}

uint32_t khr_descriptor_heap_register_texture(khr_descriptor_heap_t* heap,
                                              VkImageView view,
                                              VkImageLayout layout) {
    if (heap == nullptr || !heap->initialized || heap->texture_count >= heap->max_textures) {
        return UINT32_MAX;
    }
    uint32_t idx = heap->texture_count++;

    VkDescriptorImageInfo img_info = {
        .sampler = VK_NULL_HANDLE,
        .imageView = view,
        .imageLayout = layout,
    };
    VkDescriptorGetInfoEXT get_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
        .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        .data = { .pSampledImage = &img_info },
    };

    uint8_t* dst = (uint8_t*)heap->resource_host_map +
                   (size_t)heap->resource_binding_offset +
                   (size_t)idx * heap->resource_descriptor_size;

    heap->dev->vkGetDescriptorEXT(heap->dev->device, &get_info,
                                  heap->resource_descriptor_size, dst);
    return idx;
}

uint32_t khr_descriptor_heap_register_sampler(khr_descriptor_heap_t* heap,
                                              VkSampler sampler) {
    if (heap == nullptr || !heap->initialized || heap->sampler_count >= heap->max_samplers) {
        return UINT32_MAX;
    }
    uint32_t idx = heap->sampler_count++;

    VkDescriptorGetInfoEXT get_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
        .type = VK_DESCRIPTOR_TYPE_SAMPLER,
        .data = { .pSampler = &sampler },
    };

    uint8_t* dst = (uint8_t*)heap->sampler_host_map +
                   (size_t)heap->sampler_binding_offset +
                   (size_t)idx * heap->sampler_descriptor_size;

    heap->dev->vkGetDescriptorEXT(heap->dev->device, &get_info,
                                  heap->sampler_descriptor_size, dst);
    return idx;
}

void khr_descriptor_heap_bind(const khr_descriptor_heap_t* heap,
                              VkCommandBuffer cmd,
                              VkPipelineBindPoint bind_point,
                              VkPipelineLayout pipeline_layout) {
    if (heap == nullptr || !heap->initialized || cmd == VK_NULL_HANDLE) {
        return;
    }

    VkDescriptorBufferBindingInfoEXT bindings[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT,
            .address = heap->resource_bda,
            .usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT,
        },
        {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT,
            .address = heap->sampler_bda,
            .usage = VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT,
        },
    };
    heap->dev->vkCmdBindDescriptorBuffersEXT(cmd, 2, bindings);

    uint32_t buffer_indices[2] = { 0, 1 };
    VkDeviceSize offsets[2] = { 0, 0 };
    heap->dev->vkCmdSetDescriptorBufferOffsetsEXT(cmd, bind_point, pipeline_layout,
                                                  0, 2, buffer_indices, offsets);
}

VkDeviceAddress khr_descriptor_heap_get_resource_bda(const khr_descriptor_heap_t* heap) {
    return heap ? heap->resource_bda : 0;
}

VkDeviceAddress khr_descriptor_heap_get_sampler_bda(const khr_descriptor_heap_t* heap) {
    return heap ? heap->sampler_bda : 0;
}

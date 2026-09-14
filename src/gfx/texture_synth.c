#include "khoros/gfx/texture_synth.h"
#include "khoros/gfx/pipeline.h"
#include <string.h>

/* Embedded SPIR-V Bytecode via C23 #embed */
alignas(uint32_t) static const uint8_t khr_tex_synth_comp_spv[] = {
#embed "shaders/tex_synthesizer.comp.spv"
};

[[nodiscard]]
static uint32_t khr_synth_find_memory_type(VkPhysicalDevice phy, uint32_t type_filter,
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

static inline size_t khr_synth_align_up(size_t size, size_t alignment) {
    if (alignment == 0) return size;
    return (size + alignment - 1) & ~(alignment - 1);
}

[[nodiscard]]
bool khr_texture_synth_pipeline_init(khr_texture_synth_pipeline_t* p,
                                     const khr_gfx_device_t* dev) {
    if (p == nullptr || dev == nullptr || dev->device == VK_NULL_HANDLE) {
        return false;
    }
    if (!dev->has_descriptor_buffer || dev->vkGetDescriptorEXT == nullptr) {
        return false;
    }

    memset(p, 0, sizeof(*p));
    p->dev = dev;

    p->desc_size = (uint32_t)dev->descriptor_buffer_props.storageImageDescriptorSize;
    size_t align = (size_t)dev->descriptor_buffer_props.descriptorBufferOffsetAlignment;
    if (align == 0) align = 64;

    /* 1. Create Storage Image Descriptor Set Layout with descriptor buffer flag */
    VkDescriptorSetLayoutBinding binding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
    };
    VkDescriptorSetLayoutCreateInfo layout_ci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
        .bindingCount = 1,
        .pBindings = &binding,
    };
    if (vkCreateDescriptorSetLayout(dev->device, &layout_ci, nullptr, &p->desc_layout) != VK_SUCCESS) {
        khr_texture_synth_pipeline_destroy(p);
        return false;
    }

    dev->vkGetDescriptorSetLayoutSizeEXT(dev->device, p->desc_layout, &p->layout_size);
    dev->vkGetDescriptorSetLayoutBindingOffsetEXT(dev->device, p->desc_layout, 0, &p->binding_offset);

    /* 2. Allocate Host-Mapped Descriptor Buffer */
    size_t req_bytes = (p->layout_size > 0) ? (size_t)p->layout_size : (size_t)p->desc_size;
    p->desc_total_bytes = khr_synth_align_up(req_bytes, align);
    if (p->desc_total_bytes < 256) p->desc_total_bytes = 256;

    VkBufferCreateInfo buf_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = p->desc_total_bytes,
        .usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateBuffer(dev->device, &buf_ci, nullptr, &p->desc_buffer) != VK_SUCCESS) {
        khr_texture_synth_pipeline_destroy(p);
        return false;
    }

    VkMemoryRequirements mem_reqs = {};
    vkGetBufferMemoryRequirements(dev->device, p->desc_buffer, &mem_reqs);

    uint32_t mem_type = khr_synth_find_memory_type(
        dev->phy, mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mem_type == UINT32_MAX) {
        khr_texture_synth_pipeline_destroy(p);
        return false;
    }

    VkMemoryAllocateFlagsInfo flags_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
    };
    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &flags_info,
        .allocationSize = mem_reqs.size,
        .memoryTypeIndex = mem_type,
    };
    if (vkAllocateMemory(dev->device, &alloc_info, nullptr, &p->desc_memory) != VK_SUCCESS) {
        khr_texture_synth_pipeline_destroy(p);
        return false;
    }

    if (vkBindBufferMemory(dev->device, p->desc_buffer, p->desc_memory, 0) != VK_SUCCESS) {
        khr_texture_synth_pipeline_destroy(p);
        return false;
    }

    if (vkMapMemory(dev->device, p->desc_memory, 0, p->desc_total_bytes, 0, &p->desc_host_map) != VK_SUCCESS) {
        khr_texture_synth_pipeline_destroy(p);
        return false;
    }

    VkBufferDeviceAddressInfo bda_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = p->desc_buffer,
    };
    p->desc_bda = dev->vkGetBufferDeviceAddress(dev->device, &bda_info);

    /* 3. Create Compute Pipeline Layout */
    VkPushConstantRange pcr = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(khr_tex_synth_push_t),
    };
    VkPipelineLayoutCreateInfo pipe_layout_ci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &p->desc_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcr,
    };
    if (vkCreatePipelineLayout(dev->device, &pipe_layout_ci, nullptr, &p->layout) != VK_SUCCESS) {
        khr_texture_synth_pipeline_destroy(p);
        return false;
    }

    /* 4. Create Compute Shader Module */
    if (!khr_shader_module_create(dev->device, (const uint32_t*)(const void*)khr_tex_synth_comp_spv,
                                  sizeof(khr_tex_synth_comp_spv), &p->comp_module)) {
        khr_texture_synth_pipeline_destroy(p);
        return false;
    }

    /* 5. Create Compute Pipeline */
    VkComputePipelineCreateInfo comp_pipe_ci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = p->comp_module,
            .pName = "main",
        },
        .layout = p->layout,
    };
    if (vkCreateComputePipelines(dev->device, VK_NULL_HANDLE, 1, &comp_pipe_ci, nullptr, &p->pipeline) != VK_SUCCESS) {
        khr_texture_synth_pipeline_destroy(p);
        return false;
    }

    p->initialized = true;
    return true;
}

void khr_texture_synth_pipeline_destroy(khr_texture_synth_pipeline_t* p) {
    if (p == nullptr || p->dev == nullptr || p->dev->device == VK_NULL_HANDLE) {
        return;
    }
    VkDevice dev = p->dev->device;

    if (p->pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, p->pipeline, nullptr);
        p->pipeline = VK_NULL_HANDLE;
    }
    if (p->comp_module != VK_NULL_HANDLE) {
        khr_shader_module_destroy(dev, p->comp_module);
        p->comp_module = VK_NULL_HANDLE;
    }
    if (p->layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, p->layout, nullptr);
        p->layout = VK_NULL_HANDLE;
    }
    if (p->desc_host_map != nullptr) {
        vkUnmapMemory(dev, p->desc_memory);
        p->desc_host_map = nullptr;
    }
    if (p->desc_buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(dev, p->desc_buffer, nullptr);
        p->desc_buffer = VK_NULL_HANDLE;
    }
    if (p->desc_memory != VK_NULL_HANDLE) {
        vkFreeMemory(dev, p->desc_memory, nullptr);
        p->desc_memory = VK_NULL_HANDLE;
    }
    if (p->desc_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, p->desc_layout, nullptr);
        p->desc_layout = VK_NULL_HANDLE;
    }
    p->initialized = false;
}

void khr_texture_synth_record_dispatch(khr_texture_synth_pipeline_t* p,
                                       VkCommandBuffer cmd,
                                       khr_texture_t* tex,
                                       khr_tex_synth_pattern_t pattern,
                                       float frequency,
                                       const float tint[4]) {
    if (p == nullptr || !p->initialized || cmd == VK_NULL_HANDLE ||
        tex == nullptr || tex->image == VK_NULL_HANDLE || tex->view == VK_NULL_HANDLE) {
        return;
    }

    /* 1. Transition image to GENERAL layout for compute storage write */
    VkImageMemoryBarrier2 to_gen_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
        .srcAccessMask = VK_ACCESS_2_NONE,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .oldLayout = tex->current_layout,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = tex->image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    VkDependencyInfo to_gen_dep = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &to_gen_barrier,
    };
    vkCmdPipelineBarrier2(cmd, &to_gen_dep);
    tex->current_layout = VK_IMAGE_LAYOUT_GENERAL;

    /* 2. Direct-Memory write of Storage Image Descriptor into Descriptor Buffer */
    VkDescriptorImageInfo img_info = {
        .sampler = VK_NULL_HANDLE,
        .imageView = tex->view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    VkDescriptorGetInfoEXT get_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
        .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .data = { .pStorageImage = &img_info },
    };

    uint8_t* dst = (uint8_t*)p->desc_host_map + p->binding_offset;
    p->dev->vkGetDescriptorEXT(p->dev->device, &get_info, p->desc_size, dst);

    /* 3. Bind Descriptor Buffer */
    VkDescriptorBufferBindingInfoEXT binding_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT,
        .address = p->desc_bda,
        .usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT,
    };
    p->dev->vkCmdBindDescriptorBuffersEXT(cmd, 1, &binding_info);

    uint32_t buffer_idx = 0;
    VkDeviceSize offset = 0;
    p->dev->vkCmdSetDescriptorBufferOffsetsEXT(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                               p->layout, 0, 1, &buffer_idx, &offset);

    /* 4. Bind Compute Pipeline and Push Constants */
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipeline);

    khr_tex_synth_push_t push = {
        .width = tex->width,
        .height = tex->height,
        .pattern = (uint32_t)pattern,
        .frequency = (frequency > 0.0f) ? frequency : 8.0f,
        .tint = {
            tint ? tint[0] : 1.0f,
            tint ? tint[1] : 1.0f,
            tint ? tint[2] : 1.0f,
            tint ? tint[3] : 1.0f,
        },
    };
    vkCmdPushConstants(cmd, p->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);

    /* 5. Dispatch Compute Shader */
    uint32_t gx = (tex->width + 15U) / 16U;
    uint32_t gy = (tex->height + 15U) / 16U;
    vkCmdDispatch(cmd, gx, gy, 1);

    /* 6. Transition image to SHADER_READ_ONLY_OPTIMAL for sampler consumption */
    VkImageMemoryBarrier2 to_read_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = tex->image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    VkDependencyInfo to_read_dep = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &to_read_barrier,
    };
    vkCmdPipelineBarrier2(cmd, &to_read_dep);
    tex->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

[[nodiscard]]
bool khr_texture_synth_generate_2d(khr_texture_synth_pipeline_t* p,
                                   khr_texture_t* out_tex,
                                   uint32_t width,
                                   uint32_t height,
                                   khr_tex_synth_pattern_t pattern,
                                   float frequency,
                                   const float tint[4]) {
    if (p == nullptr || !p->initialized || out_tex == nullptr || width == 0 || height == 0) {
        return false;
    }

    VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    if (!khr_texture_create_2d(out_tex, p->dev, width, height, VK_FORMAT_R8G8B8A8_UNORM, 1, usage)) {
        return false;
    }

    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = p->dev->cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(p->dev->device, &cbai, &cmd) != VK_SUCCESS) {
        khr_texture_destroy(out_tex);
        return false;
    }

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (vkBeginCommandBuffer(cmd, &begin_info) != VK_SUCCESS) {
        vkFreeCommandBuffers(p->dev->device, p->dev->cmd_pool, 1, &cmd);
        khr_texture_destroy(out_tex);
        return false;
    }

    khr_texture_synth_record_dispatch(p, cmd, out_tex, pattern, frequency, tint);

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        vkFreeCommandBuffers(p->dev->device, p->dev->cmd_pool, 1, &cmd);
        khr_texture_destroy(out_tex);
        return false;
    }

    VkCommandBufferSubmitInfo cbsi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = cmd,
    };
    VkSubmitInfo2 submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &cbsi,
    };

    khr_gfx_device_lock_queues((khr_gfx_device_t*)p->dev);
    VkResult res = vkQueueSubmit2(p->dev->gfx_queue, 1, &submit_info, VK_NULL_HANDLE);
    if (res == VK_SUCCESS) {
        vkQueueWaitIdle(p->dev->gfx_queue);
    }
    khr_gfx_device_unlock_queues((khr_gfx_device_t*)p->dev);

    vkFreeCommandBuffers(p->dev->device, p->dev->cmd_pool, 1, &cmd);
    return (res == VK_SUCCESS);
}

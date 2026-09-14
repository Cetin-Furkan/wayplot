#include "khoros/gfx/hiz.h"
#include <string.h>

/* Embedded SPIR-V Bytecode via C23 #embed */
alignas(uint32_t) static const uint8_t khr_hiz_downsample_comp_spv[] = {
#embed "shaders/hiz_downsample.comp.spv"
};

alignas(uint32_t) static const uint8_t khr_cull_hiz_comp_spv[] = {
#embed "shaders/cull_hiz.comp.spv"
};

[[nodiscard]]
static uint32_t khr_hiz_find_memory_type(VkPhysicalDevice phy, uint32_t type_filter,
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

static void khr_hiz_image_barrier(VkCommandBuffer cmd,
                                  VkImage image,
                                  VkImageLayout old_layout,
                                  VkImageLayout new_layout,
                                  VkAccessFlags src_access,
                                  VkAccessFlags dst_access,
                                  VkPipelineStageFlags src_stage,
                                  VkPipelineStageFlags dst_stage,
                                  uint32_t base_mip,
                                  uint32_t mip_count,
                                  VkImageAspectFlags aspect) {
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {
            .aspectMask = aspect,
            .baseMipLevel = base_mip,
            .levelCount = mip_count,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

[[nodiscard]]
bool khr_hiz_init(khr_hiz_t* hiz,
                  const khr_gfx_device_t* dev,
                  uint32_t width,
                  uint32_t height) {
    if (hiz == nullptr || dev == nullptr || dev->device == VK_NULL_HANDLE ||
        width == 0 || height == 0) {
        return false;
    }
    memset(hiz, 0, sizeof(*hiz));
    hiz->dev = dev;
    hiz->width = width;
    hiz->height = height;

    /* Calculate full mip count down to 1x1 */
    uint32_t max_dim = (width > height) ? width : height;
    uint32_t mips = 1;
    while ((max_dim >> mips) > 0 && mips < KHR_HIZ_MAX_MIP_LEVELS) {
        mips++;
    }
    hiz->mip_levels = mips;

    /* 1. Create Hi-Z Pyramid Image (R32_SFLOAT) */
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R32_SFLOAT,
        .extent = { .width = width, .height = height, .depth = 1 },
        .mipLevels = hiz->mip_levels,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(dev->device, &ici, nullptr, &hiz->image) != VK_SUCCESS) {
        khr_hiz_destroy(hiz);
        return false;
    }

    VkMemoryRequirements mem_reqs = {};
    vkGetImageMemoryRequirements(dev->device, hiz->image, &mem_reqs);
    uint32_t mem_type = khr_hiz_find_memory_type(dev->phy, mem_reqs.memoryTypeBits,
                                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mem_type == UINT32_MAX) {
        khr_hiz_destroy(hiz);
        return false;
    }

    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_reqs.size,
        .memoryTypeIndex = mem_type,
    };
    if (vkAllocateMemory(dev->device, &ai, nullptr, &hiz->memory) != VK_SUCCESS) {
        khr_hiz_destroy(hiz);
        return false;
    }
    if (vkBindImageMemory(dev->device, hiz->image, hiz->memory, 0) != VK_SUCCESS) {
        khr_hiz_destroy(hiz);
        return false;
    }

    /* 2. Create Full Mip-Chain View for Sampling */
    VkImageViewCreateInfo full_vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = hiz->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R32_SFLOAT,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = hiz->mip_levels,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    if (vkCreateImageView(dev->device, &full_vci, nullptr, &hiz->view_full) != VK_SUCCESS) {
        khr_hiz_destroy(hiz);
        return false;
    }

    /* 3. Create Per-Mip Views for Compute Downsampling */
    for (uint32_t m = 0; m < hiz->mip_levels; m++) {
        VkImageViewCreateInfo mip_vci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = hiz->image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_R32_SFLOAT,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = m,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        };
        if (vkCreateImageView(dev->device, &mip_vci, nullptr, &hiz->mip_views[m]) != VK_SUCCESS) {
            khr_hiz_destroy(hiz);
            return false;
        }
    }

    /* 4. Create Reversed-Z Reduction Sampler (MIN Reduction) */
    VkSamplerReductionModeCreateInfo red_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO,
        .reductionMode = VK_SAMPLER_REDUCTION_MODE_MIN,
    };
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = &red_info,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .minLod = 0.0f,
        .maxLod = (float)hiz->mip_levels,
    };
    if (vkCreateSampler(dev->device, &sci, nullptr, &hiz->sampler) != VK_SUCCESS) {
        khr_hiz_destroy(hiz);
        return false;
    }

    /* 5. Create Descriptor Pool */
    VkDescriptorPoolSize pool_sizes[] = {
        { .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount = hiz->mip_levels + 2 },
        { .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = hiz->mip_levels + 2 },
        { .type = VK_DESCRIPTOR_TYPE_SAMPLER,       .descriptorCount = 4 },
    };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = hiz->mip_levels + 4,
        .poolSizeCount = (uint32_t)(sizeof(pool_sizes) / sizeof(pool_sizes[0])),
        .pPoolSizes = pool_sizes,
    };
    if (vkCreateDescriptorPool(dev->device, &dpci, nullptr, &hiz->desc_pool) != VK_SUCCESS) {
        khr_hiz_destroy(hiz);
        return false;
    }

    /* 6. Create Downsample Compute Pipeline */
    VkDescriptorSetLayoutBinding ds_bindings[] = {
        {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        },
        {
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    };
    VkDescriptorSetLayoutCreateInfo ds_layout_ci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings = ds_bindings,
    };
    if (vkCreateDescriptorSetLayout(dev->device, &ds_layout_ci, nullptr,
                                    &hiz->downsample_ds_layout) != VK_SUCCESS) {
        khr_hiz_destroy(hiz);
        return false;
    }

    VkPushConstantRange ds_pcr = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(uint32_t) * 2, // output_size uint2
    };
    VkPipelineLayoutCreateInfo ds_pipe_layout_ci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &hiz->downsample_ds_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &ds_pcr,
    };
    if (vkCreatePipelineLayout(dev->device, &ds_pipe_layout_ci, nullptr,
                               &hiz->downsample_pipe_layout) != VK_SUCCESS) {
        khr_hiz_destroy(hiz);
        return false;
    }

    VkShaderModuleCreateInfo ds_smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(khr_hiz_downsample_comp_spv),
        .pCode = (const uint32_t*)(const void*)khr_hiz_downsample_comp_spv,
    };
    if (vkCreateShaderModule(dev->device, &ds_smci, nullptr, &hiz->downsample_module) != VK_SUCCESS) {
        khr_hiz_destroy(hiz);
        return false;
    }

    VkComputePipelineCreateInfo ds_cpci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = hiz->downsample_module,
            .pName = "main",
        },
        .layout = hiz->downsample_pipe_layout,
    };
    if (vkCreateComputePipelines(dev->device, VK_NULL_HANDLE, 1, &ds_cpci, nullptr,
                                 &hiz->downsample_pipeline) != VK_SUCCESS) {
        khr_hiz_destroy(hiz);
        return false;
    }

    /* Allocate Downsample Descriptor Sets */
    for (uint32_t m = 1; m < hiz->mip_levels; m++) {
        VkDescriptorSetAllocateInfo ds_ai = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = hiz->desc_pool,
            .descriptorSetCount = 1,
            .pSetLayouts = &hiz->downsample_ds_layout,
        };
        if (vkAllocateDescriptorSets(dev->device, &ds_ai, &hiz->downsample_sets[m]) != VK_SUCCESS) {
            khr_hiz_destroy(hiz);
            return false;
        }

        VkDescriptorImageInfo in_info = {
            .imageView = hiz->mip_views[m - 1],
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        VkDescriptorImageInfo out_info = {
            .imageView = hiz->mip_views[m],
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        VkWriteDescriptorSet writes[] = {
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = hiz->downsample_sets[m],
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                .pImageInfo = &in_info,
            },
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = hiz->downsample_sets[m],
                .dstBinding = 1,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &out_info,
            },
        };
        vkUpdateDescriptorSets(dev->device, 2, writes, 0, nullptr);
    }

    /* 7. Create Hi-Z Occlusion Culling Compute Pipeline */
    VkDescriptorSetLayoutBinding cull_bindings[] = {
        {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        },
        {
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    };
    VkDescriptorSetLayoutCreateInfo cull_ds_ci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings = cull_bindings,
    };
    if (vkCreateDescriptorSetLayout(dev->device, &cull_ds_ci, nullptr,
                                    &hiz->cull_ds_layout) != VK_SUCCESS) {
        khr_hiz_destroy(hiz);
        return false;
    }

    VkPushConstantRange cull_pcr = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(khr_hiz_cull_push_t),
    };
    VkPipelineLayoutCreateInfo cull_plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &hiz->cull_ds_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &cull_pcr,
    };
    if (vkCreatePipelineLayout(dev->device, &cull_plci, nullptr,
                               &hiz->cull_pipe_layout) != VK_SUCCESS) {
        khr_hiz_destroy(hiz);
        return false;
    }

    VkShaderModuleCreateInfo cull_smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(khr_cull_hiz_comp_spv),
        .pCode = (const uint32_t*)(const void*)khr_cull_hiz_comp_spv,
    };
    if (vkCreateShaderModule(dev->device, &cull_smci, nullptr, &hiz->cull_module) != VK_SUCCESS) {
        khr_hiz_destroy(hiz);
        return false;
    }

    VkComputePipelineCreateInfo cull_cpci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = hiz->cull_module,
            .pName = "main",
        },
        .layout = hiz->cull_pipe_layout,
    };
    if (vkCreateComputePipelines(dev->device, VK_NULL_HANDLE, 1, &cull_cpci, nullptr,
                                 &hiz->cull_pipeline) != VK_SUCCESS) {
        khr_hiz_destroy(hiz);
        return false;
    }

    /* Allocate and update cull descriptor set */
    VkDescriptorSetAllocateInfo cull_ai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = hiz->desc_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &hiz->cull_ds_layout,
    };
    if (vkAllocateDescriptorSets(dev->device, &cull_ai, &hiz->cull_set) != VK_SUCCESS) {
        khr_hiz_destroy(hiz);
        return false;
    }

    VkDescriptorImageInfo cull_img_info = {
        .imageView = hiz->view_full,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkDescriptorImageInfo cull_samp_info = {
        .sampler = hiz->sampler,
    };
    VkWriteDescriptorSet cull_writes[] = {
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = hiz->cull_set,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            .pImageInfo = &cull_img_info,
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = hiz->cull_set,
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
            .pImageInfo = &cull_samp_info,
        },
    };
    vkUpdateDescriptorSets(dev->device, 2, cull_writes, 0, nullptr);

    hiz->initialized = true;
    return true;
}

void khr_hiz_destroy(khr_hiz_t* hiz) {
    if (hiz == nullptr || hiz->dev == nullptr) {
        return;
    }
    VkDevice dev = hiz->dev->device;
    if (dev == VK_NULL_HANDLE) {
        return;
    }

    if (hiz->cull_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, hiz->cull_pipeline, nullptr);
        hiz->cull_pipeline = VK_NULL_HANDLE;
    }
    if (hiz->cull_pipe_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, hiz->cull_pipe_layout, nullptr);
        hiz->cull_pipe_layout = VK_NULL_HANDLE;
    }
    if (hiz->cull_module != VK_NULL_HANDLE) {
        vkDestroyShaderModule(dev, hiz->cull_module, nullptr);
        hiz->cull_module = VK_NULL_HANDLE;
    }
    if (hiz->cull_ds_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, hiz->cull_ds_layout, nullptr);
        hiz->cull_ds_layout = VK_NULL_HANDLE;
    }

    if (hiz->downsample_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, hiz->downsample_pipeline, nullptr);
        hiz->downsample_pipeline = VK_NULL_HANDLE;
    }
    if (hiz->downsample_pipe_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, hiz->downsample_pipe_layout, nullptr);
        hiz->downsample_pipe_layout = VK_NULL_HANDLE;
    }
    if (hiz->downsample_module != VK_NULL_HANDLE) {
        vkDestroyShaderModule(dev, hiz->downsample_module, nullptr);
        hiz->downsample_module = VK_NULL_HANDLE;
    }
    if (hiz->downsample_ds_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, hiz->downsample_ds_layout, nullptr);
        hiz->downsample_ds_layout = VK_NULL_HANDLE;
    }

    if (hiz->desc_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(dev, hiz->desc_pool, nullptr);
        hiz->desc_pool = VK_NULL_HANDLE;
    }
    if (hiz->sampler != VK_NULL_HANDLE) {
        vkDestroySampler(dev, hiz->sampler, nullptr);
        hiz->sampler = VK_NULL_HANDLE;
    }
    for (uint32_t m = 0; m < KHR_HIZ_MAX_MIP_LEVELS; m++) {
        if (hiz->mip_views[m] != VK_NULL_HANDLE) {
            vkDestroyImageView(dev, hiz->mip_views[m], nullptr);
            hiz->mip_views[m] = VK_NULL_HANDLE;
        }
    }
    if (hiz->view_full != VK_NULL_HANDLE) {
        vkDestroyImageView(dev, hiz->view_full, nullptr);
        hiz->view_full = VK_NULL_HANDLE;
    }
    if (hiz->image != VK_NULL_HANDLE) {
        vkDestroyImage(dev, hiz->image, nullptr);
        hiz->image = VK_NULL_HANDLE;
    }
    if (hiz->memory != VK_NULL_HANDLE) {
        vkFreeMemory(dev, hiz->memory, nullptr);
        hiz->memory = VK_NULL_HANDLE;
    }

    hiz->initialized = false;
}

void khr_hiz_build(const khr_hiz_t* hiz,
                   VkCommandBuffer cmd,
                   VkImage src_depth,
                   VkImageLayout current_depth_layout) {
    if (hiz == nullptr || !hiz->initialized || cmd == VK_NULL_HANDLE ||
        src_depth == VK_NULL_HANDLE) {
        return;
    }

    /* 1. Transition src depth to TRANSFER_SRC */
    khr_hiz_image_barrier(cmd, src_depth,
                          current_depth_layout,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                          VK_ACCESS_TRANSFER_READ_BIT,
                          VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                          0, 1, VK_IMAGE_ASPECT_DEPTH_BIT);

    /* 2. Transition Hi-Z mip 0 to TRANSFER_DST */
    khr_hiz_image_barrier(cmd, hiz->image,
                          VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          0, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                          0, 1, VK_IMAGE_ASPECT_COLOR_BIT);

    /* 3. Copy Depth Buffer into Hi-Z Mip 0 */
    VkImageCopy copy_region = {
        .srcSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .srcOffset = { 0, 0, 0 },
        .dstSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .dstOffset = { 0, 0, 0 },
        .extent = { .width = hiz->width, .height = hiz->height, .depth = 1 },
    };
    vkCmdCopyImage(cmd, src_depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   hiz->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &copy_region);

    /* 4. Restore src depth to its previous layout */
    khr_hiz_image_barrier(cmd, src_depth,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          current_depth_layout,
                          VK_ACCESS_TRANSFER_READ_BIT,
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                          0, 1, VK_IMAGE_ASPECT_DEPTH_BIT);

    /* 5. Downsample Pyramid Loop: Mip k-1 -> Mip k */
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hiz->downsample_pipeline);

    for (uint32_t m = 1; m < hiz->mip_levels; m++) {
        /* Transition mip m-1 to SHADER_READ_ONLY */
        khr_hiz_image_barrier(cmd, hiz->image,
                              (m == 1) ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                                       : VK_IMAGE_LAYOUT_GENERAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              (m == 1) ? VK_ACCESS_TRANSFER_WRITE_BIT
                                       : VK_ACCESS_SHADER_WRITE_BIT,
                              VK_ACCESS_SHADER_READ_BIT,
                              (m == 1) ? VK_PIPELINE_STAGE_TRANSFER_BIT
                                       : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                              m - 1, 1, VK_IMAGE_ASPECT_COLOR_BIT);

        /* Transition mip m to GENERAL (Storage write) */
        khr_hiz_image_barrier(cmd, hiz->image,
                              VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_GENERAL,
                              0, VK_ACCESS_SHADER_WRITE_BIT,
                              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                              m, 1, VK_IMAGE_ASPECT_COLOR_BIT);

        uint32_t out_w = (hiz->width >> m) > 0 ? (hiz->width >> m) : 1;
        uint32_t out_h = (hiz->height >> m) > 0 ? (hiz->height >> m) : 1;

        uint32_t push[2] = { out_w, out_h };
        vkCmdPushConstants(cmd, hiz->downsample_pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(push), push);

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                hiz->downsample_pipe_layout, 0, 1,
                                &hiz->downsample_sets[m], 0, nullptr);

        uint32_t groups_x = (out_w + 7) / 8;
        uint32_t groups_y = (out_h + 7) / 8;
        vkCmdDispatch(cmd, groups_x, groups_y, 1);
    }

    /* 6. Transition final mip to SHADER_READ_ONLY */
    uint32_t last_mip = hiz->mip_levels - 1;
    khr_hiz_image_barrier(cmd, hiz->image,
                          VK_IMAGE_LAYOUT_GENERAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_ACCESS_SHADER_WRITE_BIT,
                          VK_ACCESS_SHADER_READ_BIT,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          last_mip, 1, VK_IMAGE_ASPECT_COLOR_BIT);
}

void khr_hiz_cull_dispatch(const khr_hiz_t* hiz,
                           VkCommandBuffer cmd,
                           const khr_hiz_cull_push_t* push) {
    if (hiz == nullptr || !hiz->initialized || cmd == VK_NULL_HANDLE || push == nullptr) {
        return;
    }
    if (push->instance_count == 0) {
        return;
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hiz->cull_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            hiz->cull_pipe_layout, 0, 1,
                            &hiz->cull_set, 0, nullptr);

    vkCmdPushConstants(cmd, hiz->cull_pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(*push), push);

    uint32_t groups = (push->instance_count + 63) / 64;
    vkCmdDispatch(cmd, groups, 1, 1);
}

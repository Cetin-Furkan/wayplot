#include "khoros/gfx/dmabuf.h"
#include "khoros/gfx/blob.h"

#include <string.h>
#include <unistd.h>
#include <drm/drm_fourcc.h>

[[nodiscard]]
static uint32_t khr_dmabuf_mem_type(VkPhysicalDevice phy, uint32_t filter,
                                    VkMemoryPropertyFlags req) {
    VkPhysicalDeviceMemoryProperties props = {};
    vkGetPhysicalDeviceMemoryProperties(phy, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if ((filter & (1U << i)) &&
            (props.memoryTypes[i].propertyFlags & req) == req) {
            return i;
        }
    }
    return UINT32_MAX;
}

[[nodiscard]]
static bool khr_slot_local_image(khr_gfx_device_t* d, uint32_t w, uint32_t h,
                                 VkFormat format, VkSampleCountFlagBits samples,
                                 VkImageUsageFlags usage, VkImageAspectFlags aspect,
                                 VkImage* image, VkDeviceMemory* mem, VkImageView* view) {
    if (d == nullptr || image == nullptr || mem == nullptr || view == nullptr) {
        return false;
    }
    *image = VK_NULL_HANDLE;
    *mem = VK_NULL_HANDLE;
    *view = VK_NULL_HANDLE;
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { .width = w, .height = h, .depth = 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = samples,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(d->device, &ici, nullptr, image) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req = {};
    vkGetImageMemoryRequirements(d->device, *image, &req);
    uint32_t mem_idx = khr_dmabuf_mem_type(d->phy, req.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mem_idx == UINT32_MAX) {
        mem_idx = khr_dmabuf_mem_type(d->phy, req.memoryTypeBits, 0);
    }
    if (mem_idx == UINT32_MAX) {
        vkDestroyImage(d->device, *image, nullptr);
        *image = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = mem_idx,
    };
    if (vkAllocateMemory(d->device, &ai, nullptr, mem) != VK_SUCCESS) {
        vkDestroyImage(d->device, *image, nullptr);
        *image = VK_NULL_HANDLE;
        return false;
    }
    if (vkBindImageMemory(d->device, *image, *mem, 0) != VK_SUCCESS) {
        vkFreeMemory(d->device, *mem, nullptr);
        vkDestroyImage(d->device, *image, nullptr);
        *image = VK_NULL_HANDLE;
        *mem = VK_NULL_HANDLE;
        return false;
    }
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = *image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = {
            .aspectMask = aspect,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    if (vkCreateImageView(d->device, &vci, nullptr, view) != VK_SUCCESS) {
        vkFreeMemory(d->device, *mem, nullptr);
        vkDestroyImage(d->device, *image, nullptr);
        *image = VK_NULL_HANDLE;
        *mem = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

static void khr_slot_local_image_destroy(khr_gfx_device_t* d, VkImage* image,
                                         VkDeviceMemory* mem, VkImageView* view) {
    if (d == nullptr || d->device == VK_NULL_HANDLE) {
        return;
    }
    if (view != nullptr && *view != VK_NULL_HANDLE) {
        vkDestroyImageView(d->device, *view, nullptr);
        *view = VK_NULL_HANDLE;
    }
    if (image != nullptr && *image != VK_NULL_HANDLE) {
        vkDestroyImage(d->device, *image, nullptr);
        *image = VK_NULL_HANDLE;
    }
    if (mem != nullptr && *mem != VK_NULL_HANDLE) {
        vkFreeMemory(d->device, *mem, nullptr);
        *mem = VK_NULL_HANDLE;
    }
}

[[nodiscard]]
bool khr_dmabuf_image_init_with_modifiers(khr_gfx_device_t* d, khr_dmabuf_image_t* img,
                                          uint32_t w, uint32_t h,
                                          const uint64_t* wayland_modifiers,
                                          uint32_t wayland_modifier_count) {
    if (d == nullptr || d->device == VK_NULL_HANDLE || img == nullptr) {
        return false;
    }
    if (w == 0 || h == 0 || w > 4'096 || h > 4'096) {
        return false;
    }
    *img = (khr_dmabuf_image_t){ .w = w, .h = h, .dma_fd = -1 };

    PFN_vkGetImageDrmFormatModifierPropertiesEXT pfn_mod =
        d->vkGetImageDrmFormatModifierPropertiesEXT
            ? d->vkGetImageDrmFormatModifierPropertiesEXT
            : (PFN_vkGetImageDrmFormatModifierPropertiesEXT)
                  vkGetDeviceProcAddr(d->device, "vkGetImageDrmFormatModifierPropertiesEXT");

    /* Attempt 1: Intersect compositor modifiers against driver-supported DRM format modifiers */
    if (pfn_mod != nullptr) {
        VkDrmFormatModifierPropertiesListEXT mod_props_list = {
            .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
        };
        VkFormatProperties2 fmt_props2 = {
            .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
            .pNext = &mod_props_list,
        };
        vkGetPhysicalDeviceFormatProperties2(d->phy, KHR_DMABUF_VK_FORMAT, &fmt_props2);

        constexpr uint32_t MAX_VK_MODS = 128;
        uint32_t vk_count = mod_props_list.drmFormatModifierCount;
        if (vk_count > 0) {
            if (vk_count > MAX_VK_MODS) {
                vk_count = MAX_VK_MODS;
            }
            VkDrmFormatModifierPropertiesEXT vk_mods[MAX_VK_MODS] = {};
            mod_props_list.drmFormatModifierCount = vk_count;
            mod_props_list.pDrmFormatModifierProperties = vk_mods;
            vkGetPhysicalDeviceFormatProperties2(d->phy, KHR_DMABUF_VK_FORMAT, &fmt_props2);

            /* Intersect candidate list against compositor advertised modifiers */
            uint64_t candidate_mods[MAX_VK_MODS] = {};
            uint32_t candidate_count = 0;

            if (wayland_modifiers != nullptr && wayland_modifier_count > 0) {
                for (uint32_t wm = 0; wm < wayland_modifier_count; wm++) {
                    uint64_t wmod = wayland_modifiers[wm];
                    if (wmod == DRM_FORMAT_MOD_INVALID) {
                        continue;
                    }
                    for (uint32_t vm = 0; vm < vk_count; vm++) {
                        if (vk_mods[vm].drmFormatModifier == wmod &&
                            vk_mods[vm].drmFormatModifierPlaneCount == 1 &&
                            (vk_mods[vm].drmFormatModifierTilingFeatures &
                             VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0) {
                            bool exists = false;
                            for (uint32_t c = 0; c < candidate_count; c++) {
                                if (candidate_mods[c] == wmod) {
                                    exists = true;
                                    break;
                                }
                            }
                            if (!exists && candidate_count < MAX_VK_MODS) {
                                candidate_mods[candidate_count++] = wmod;
                            }
                            break;
                        }
                    }
                }
            } else {
                /* No compositor modifier list specified: consider all driver single-plane color attachment modifiers */
                for (uint32_t vm = 0; vm < vk_count; vm++) {
                    if (vk_mods[vm].drmFormatModifierPlaneCount == 1 &&
                        (vk_mods[vm].drmFormatModifierTilingFeatures &
                         VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0) {
                        uint64_t mod = vk_mods[vm].drmFormatModifier;
                        if (mod != DRM_FORMAT_MOD_INVALID && candidate_count < MAX_VK_MODS) {
                            candidate_mods[candidate_count++] = mod;
                        }
                    }
                }
            }

            if (candidate_count > 0) {
                VkExternalMemoryImageCreateInfo ext_ci = {
                    .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
                    .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                };
                VkImageDrmFormatModifierListCreateInfoEXT mod_list_ci = {
                    .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
                    .pNext = &ext_ci,
                    .drmFormatModifierCount = candidate_count,
                    .pDrmFormatModifiers = candidate_mods,
                };
                VkImageCreateInfo ici = {
                    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                    .pNext = &mod_list_ci,
                    .imageType = VK_IMAGE_TYPE_2D,
                    .format = KHR_DMABUF_VK_FORMAT,
                    .extent = { .width = w, .height = h, .depth = 1 },
                    .mipLevels = 1,
                    .arrayLayers = 1,
                    .samples = VK_SAMPLE_COUNT_1_BIT,
                    .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                    .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                };
                if (vkCreateImage(d->device, &ici, nullptr, &img->image) == VK_SUCCESS) {
                    VkMemoryRequirements req = {};
                    vkGetImageMemoryRequirements(d->device, img->image, &req);
                    uint32_t mem_idx = khr_dmabuf_mem_type(d->phy, req.memoryTypeBits,
                                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                    if (mem_idx == UINT32_MAX) {
                        mem_idx = khr_dmabuf_mem_type(d->phy, req.memoryTypeBits, 0);
                    }
                    if (mem_idx != UINT32_MAX) {
                        VkExportMemoryAllocateInfo export_ai = {
                            .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
                            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                        };
                        VkMemoryAllocateInfo ai = {
                            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                            .pNext = &export_ai,
                            .allocationSize = req.size,
                            .memoryTypeIndex = mem_idx,
                        };
                        if (vkAllocateMemory(d->device, &ai, nullptr, &img->mem) == VK_SUCCESS &&
                            vkBindImageMemory(d->device, img->image, img->mem, 0) == VK_SUCCESS) {
                            VkImageDrmFormatModifierPropertiesEXT mod_props = {
                                .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT,
                            };
                            if (pfn_mod(d->device, img->image, &mod_props) == VK_SUCCESS) {
                                img->modifier = mod_props.drmFormatModifier;
                                img->tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
                                VkImageSubresource sub = {
                                    .aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,
                                };
                                VkSubresourceLayout layout = {};
                                vkGetImageSubresourceLayout(d->device, img->image, &sub, &layout);
                                img->stride = (uint32_t)layout.rowPitch;
                                img->offset = (uint32_t)layout.offset;
                                img->drm_format = DRM_FORMAT_ARGB8888;
                                return true;
                            }
                        }
                    }
                    khr_dmabuf_image_destroy(d, img);
                    *img = (khr_dmabuf_image_t){ .w = w, .h = h, .dma_fd = -1 };
                }
            }
        }
    }

    /* Attempt 2: Fallback to optimal/linear exportability probe */
    static const VkImageTiling try_tilings[2] = {
        VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_TILING_LINEAR,
    };
    VkImageTiling chosen = VK_IMAGE_TILING_OPTIMAL;
    bool exportable = false;
    for (uint32_t i = 0; i < 2; i++) {
        VkPhysicalDeviceExternalImageFormatInfo ext_info = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        };
        VkPhysicalDeviceImageFormatInfo2 fmt_info = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
            .pNext = &ext_info,
            .format = KHR_DMABUF_VK_FORMAT,
            .type = VK_IMAGE_TYPE_2D,
            .tiling = try_tilings[i],
            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        };
        VkExternalImageFormatProperties ext_props = {
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
        };
        VkImageFormatProperties2 fmt_props = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
            .pNext = &ext_props,
        };
        if (vkGetPhysicalDeviceImageFormatProperties2(d->phy, &fmt_info, &fmt_props) == VK_SUCCESS &&
            (ext_props.externalMemoryProperties.externalMemoryFeatures &
             VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0) {
            chosen = try_tilings[i];
            exportable = true;
            break;
        }
    }
    if (!exportable) {
        return false;
    }
    img->tiling = chosen;

    VkExternalMemoryImageCreateInfo ext_ci = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &ext_ci,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = KHR_DMABUF_VK_FORMAT,
        .extent = { .width = w, .height = h, .depth = 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = chosen,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(d->device, &ici, nullptr, &img->image) != VK_SUCCESS) {
        khr_dmabuf_image_destroy(d, img);
        return false;
    }
    VkMemoryRequirements req = {};
    vkGetImageMemoryRequirements(d->device, img->image, &req);
    uint32_t mem_idx = khr_dmabuf_mem_type(d->phy, req.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mem_idx == UINT32_MAX) {
        mem_idx = khr_dmabuf_mem_type(d->phy, req.memoryTypeBits, 0);
    }
    if (mem_idx == UINT32_MAX) {
        khr_dmabuf_image_destroy(d, img);
        return false;
    }
    VkExportMemoryAllocateInfo export_ai = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &export_ai,
        .allocationSize = req.size,
        .memoryTypeIndex = mem_idx,
    };
    if (vkAllocateMemory(d->device, &ai, nullptr, &img->mem) != VK_SUCCESS) {
        khr_dmabuf_image_destroy(d, img);
        return false;
    }
    if (vkBindImageMemory(d->device, img->image, img->mem, 0) != VK_SUCCESS) {
        khr_dmabuf_image_destroy(d, img);
        return false;
    }

    if (pfn_mod != nullptr) {
        VkImageDrmFormatModifierPropertiesEXT mod_props = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT,
        };
        if (pfn_mod(d->device, img->image, &mod_props) == VK_SUCCESS) {
            img->modifier = mod_props.drmFormatModifier;
        }
    }
    if (img->modifier == DRM_FORMAT_MOD_INVALID && img->tiling == VK_IMAGE_TILING_LINEAR) {
        img->modifier = DRM_FORMAT_MOD_LINEAR;
    }

    VkImageSubresource sub = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
    };
    VkSubresourceLayout layout = {};
    vkGetImageSubresourceLayout(d->device, img->image, &sub, &layout);
    img->stride = (uint32_t)layout.rowPitch;
    img->offset = (uint32_t)layout.offset;
    img->drm_format = DRM_FORMAT_ARGB8888;
    return true;
}

[[nodiscard]]
bool khr_dmabuf_image_init(khr_gfx_device_t* d, khr_dmabuf_image_t* img,
                           uint32_t w, uint32_t h) {
    return khr_dmabuf_image_init_with_modifiers(d, img, w, h, nullptr, 0);
}

[[nodiscard]]
bool khr_dmabuf_image_export(khr_gfx_device_t* d, khr_dmabuf_image_t* img) {
    if (d == nullptr || d->device == VK_NULL_HANDLE || img == nullptr ||
        img->mem == VK_NULL_HANDLE || img->dma_fd >= 0) {
        return false;
    }
    PFN_vkGetMemoryFdKHR pfn_fd = (PFN_vkGetMemoryFdKHR)
        vkGetDeviceProcAddr(d->device, "vkGetMemoryFdKHR");
    if (pfn_fd == nullptr) {
        return false;
    }
    VkMemoryGetFdInfoKHR fd_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .memory = img->mem,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    int fd = -1;
    if (pfn_fd(d->device, &fd_info, &fd) != VK_SUCCESS || fd < 0) {
        return false;
    }
    img->dma_fd = fd;
    return true;
}

void khr_dmabuf_image_destroy(khr_gfx_device_t* d, khr_dmabuf_image_t* img) {
    if (d == nullptr || img == nullptr) {
        return;
    }
    if (img->dma_fd > 0) {
        close(img->dma_fd);
    }
    if (d->device != VK_NULL_HANDLE) {
        if (img->image != VK_NULL_HANDLE) {
            vkDestroyImage(d->device, img->image, nullptr);
        }
        if (img->mem != VK_NULL_HANDLE) {
            vkFreeMemory(d->device, img->mem, nullptr);
        }
    }
    *img = (khr_dmabuf_image_t){ .dma_fd = -1 };
}

[[nodiscard]]
bool khr_dmabuf_slot_init_shared_with_modifiers(khr_gfx_device_t* d, khr_dmabuf_slot_t* slot,
                                                uint32_t w, uint32_t h,
                                                const khr_card_pipeline_t* shared_pipe,
                                                const uint64_t* modifiers,
                                                uint32_t modifier_count) {
    if (d == nullptr || d->device == VK_NULL_HANDLE || slot == nullptr ||
        w == 0 || h == 0 || w > 4'096 || h > 4'096) {
        return false;
    }
    VkCommandPool existing_pool = slot->pool;
    VkCommandBuffer existing_cmd = slot->cmd;
    *slot = (khr_dmabuf_slot_t){ .layout = VK_IMAGE_LAYOUT_UNDEFINED };
    slot->pool = existing_pool;
    slot->cmd = existing_cmd;

    if (!khr_dmabuf_image_init_with_modifiers(d, &slot->img, w, h, modifiers, modifier_count) ||
        !khr_dmabuf_image_export(d, &slot->img)) {
        khr_dmabuf_slot_destroy(d, slot);
        return false;
    }
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = slot->img.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = KHR_DMABUF_VK_FORMAT,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    if (vkCreateImageView(d->device, &vci, nullptr, &slot->view) != VK_SUCCESS) {
        khr_dmabuf_slot_destroy(d, slot);
        return false;
    }
    if (slot->pool == VK_NULL_HANDLE) {
        VkCommandPoolCreateInfo pci = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = d->gfx_family,
        };
        if (vkCreateCommandPool(d->device, &pci, nullptr, &slot->pool) != VK_SUCCESS) {
            khr_dmabuf_slot_destroy(d, slot);
            return false;
        }
        VkCommandBufferAllocateInfo aci = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = slot->pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        if (vkAllocateCommandBuffers(d->device, &aci, &slot->cmd) != VK_SUCCESS) {
            khr_dmabuf_slot_destroy(d, slot);
            return false;
        }
    } else {
        (void)vkResetCommandPool(d->device, slot->pool, 0);
    }
    if (shared_pipe != nullptr && shared_pipe->pipeline != VK_NULL_HANDLE) {
        slot->pipe = *shared_pipe;
        slot->pipe_live = true;
        slot->owns_pipe = false;
    } else {
        if (!khr_card_pipeline_init(&slot->pipe, d, KHR_DMABUF_VK_FORMAT)) {
            khr_dmabuf_slot_destroy(d, slot);
            return false;
        }
        slot->pipe_live = true;
        slot->owns_pipe = true;
    }
    slot->samples = khr_gfx_sample_count(d);
    slot->depth_format = khr_gfx_depth_format(d);
    slot->msaa_color_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    slot->depth_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (slot->samples != VK_SAMPLE_COUNT_1_BIT) {
        if (!khr_slot_local_image(d, w, h, KHR_DMABUF_VK_FORMAT, slot->samples,
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                  VK_IMAGE_ASPECT_COLOR_BIT,
                                  &slot->msaa_color, &slot->msaa_color_mem,
                                  &slot->msaa_color_view)) {
            khr_dmabuf_slot_destroy(d, slot);
            return false;
        }
    }
    if (slot->depth_format != VK_FORMAT_UNDEFINED) {
        if (!khr_depth_target_create(d, &slot->depth_target, w, h, slot->samples,
                                     slot->depth_format)) {
            khr_dmabuf_slot_destroy(d, slot);
            return false;
        }
    }
    slot->pfn_push2 = (PFN_vkCmdPushConstants2KHR)
        vkGetDeviceProcAddr(d->device, "vkCmdPushConstants2KHR");
    if (d->has_timestamps) {
        VkQueryPoolCreateInfo qpci = {
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = 2,
        };
        if (vkCreateQueryPool(d->device, &qpci, nullptr, &slot->query_pool) == VK_SUCCESS) {
            slot->has_query_pool = true;
        }
    }
    return true;
}

[[nodiscard]]
bool khr_dmabuf_slot_init_shared(khr_gfx_device_t* d, khr_dmabuf_slot_t* slot,
                                 uint32_t w, uint32_t h,
                                 const khr_card_pipeline_t* shared_pipe) {
    return khr_dmabuf_slot_init_shared_with_modifiers(d, slot, w, h, shared_pipe, nullptr, 0);
}

[[nodiscard]]
bool khr_dmabuf_slot_init(khr_gfx_device_t* d, khr_dmabuf_slot_t* slot,
                          uint32_t w, uint32_t h) {
    return khr_dmabuf_slot_init_shared(d, slot, w, h, nullptr);
}

void khr_dmabuf_slot_destroy(khr_gfx_device_t* d, khr_dmabuf_slot_t* slot) {
    if (d == nullptr || slot == nullptr) {
        return;
    }
    if (d->device != VK_NULL_HANDLE) {
        if (slot->pipe_live && slot->owns_pipe) {
            khr_card_pipeline_destroy(&slot->pipe);
            slot->pipe_live = false;
        }
        if (slot->pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(d->device, slot->pool, nullptr);
            slot->pool = VK_NULL_HANDLE;
        }
        if (slot->view != VK_NULL_HANDLE) {
            vkDestroyImageView(d->device, slot->view, nullptr);
            slot->view = VK_NULL_HANDLE;
        }
        khr_slot_local_image_destroy(d, &slot->msaa_color, &slot->msaa_color_mem,
                                     &slot->msaa_color_view);
        khr_depth_target_destroy(d, &slot->depth_target);
        if (slot->has_query_pool && slot->query_pool != VK_NULL_HANDLE) {
            vkDestroyQueryPool(d->device, slot->query_pool, nullptr);
            slot->query_pool = VK_NULL_HANDLE;
            slot->has_query_pool = false;
        }
    }
    khr_dmabuf_image_destroy(d, &slot->img);
    slot->cmd = VK_NULL_HANDLE;
    slot->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    slot->painted = 0;
}

[[nodiscard]]
bool khr_dmabuf_slot_render_cards(khr_gfx_device_t* d, khr_dmabuf_slot_t* slot,
                                  VkDeviceAddress cards_addr, uint32_t card_count,
                                  VkSemaphore signal_sem, uint64_t signal_value) {
    return khr_dmabuf_slot_render_scene(d, slot, cards_addr, card_count,
                                        nullptr, nullptr, nullptr, nullptr,
                                        nullptr, nullptr, 0, signal_sem, signal_value);
}

[[nodiscard]]
bool khr_dmabuf_slot_render_scene(khr_gfx_device_t* d, khr_dmabuf_slot_t* slot,
                                  VkDeviceAddress cards_addr, uint32_t card_count,
                                  const khr_plot_pipeline_t* plot,
                                  const khr_plot_push_t* plot_push,
                                  const khr_mesh_pipeline_t* mesh,
                                  const khr_mesh_push_t* mesh_push,
                                  const khr_gpu_scene_pass_t* gpu_scene,
                                  const khr_gizmo_pass_t* gizmo,
                                  uint32_t plot_top_px,
                                  VkSemaphore signal_sem, uint64_t signal_value) {
    if (d == nullptr || d->device == VK_NULL_HANDLE || slot == nullptr ||
        slot->cmd == VK_NULL_HANDLE || !slot->pipe_live ||
        signal_sem == VK_NULL_HANDLE || signal_value == 0 ||
        cards_addr == 0 || card_count == 0 || card_count > 16) {
        return false;
    }
    uint32_t w = slot->img.w;
    uint32_t h = slot->img.h;
    khr_card_push_t push = {
        .cards_addr = cards_addr,
        .screen_extent = { (float)w, (float)h },
        .scale = 1.0f,
        .card_index = 0,
    };

    if (vkResetCommandPool(d->device, slot->pool, 0) != VK_SUCCESS) {
        return false;
    }
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (vkBeginCommandBuffer(slot->cmd, &begin) != VK_SUCCESS) {
        return false;
    }

    if (slot->has_query_pool) {
        vkCmdResetQueryPool(slot->cmd, slot->query_pool, 0, 2);
        vkCmdWriteTimestamp(slot->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, slot->query_pool, 0);
    }

    VkMemoryBarrier2 host_bar = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
        .srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                        VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                         VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                         VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
    };
    VkDependencyInfo dep0 = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &host_bar,
    };
    vkCmdPipelineBarrier2(slot->cmd, &dep0);

    /* 0b. GPU Compute Frustum Culling Dispatch (Pillar A) or Hi-Z Occlusion Culling (Pillar 5) */
    if (gpu_scene != nullptr) {
        if (gpu_scene->hiz != nullptr && gpu_scene->hiz_push != nullptr &&
            gpu_scene->hiz_push->instance_count > 0) {
            if (slot->depth_target.view != VK_NULL_HANDLE &&
                slot->depth_layout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL) {
                khr_hiz_build(gpu_scene->hiz, slot->cmd, slot->depth_target.image,
                              VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
            }
            khr_hiz_cull_dispatch(gpu_scene->hiz, slot->cmd, gpu_scene->hiz_push);

            VkMemoryBarrier2 cull_bar = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                                VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                .dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT |
                                 VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            };
            VkDependencyInfo cull_dep = {
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .memoryBarrierCount = 1,
                .pMemoryBarriers = &cull_bar,
            };
            vkCmdPipelineBarrier2(slot->cmd, &cull_dep);
        } else if (gpu_scene->cull_pipe != nullptr &&
                   gpu_scene->cull_pipe->pipeline != VK_NULL_HANDLE &&
                   gpu_scene->cull_push != nullptr && gpu_scene->cull_push->instance_count > 0) {
            khr_cull_pipeline_dispatch(gpu_scene->cull_pipe, slot->cmd, gpu_scene->cull_push);

            VkMemoryBarrier2 cull_bar = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                                VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                .dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT |
                                 VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            };
            VkDependencyInfo cull_dep = {
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .memoryBarrierCount = 1,
                .pMemoryBarriers = &cull_bar,
            };
            vkCmdPipelineBarrier2(slot->cmd, &cull_dep);
        }
    }

    bool msaa = slot->samples != VK_SAMPLE_COUNT_1_BIT &&
                slot->msaa_color_view != VK_NULL_HANDLE;
    VkImageMemoryBarrier2 bars[3] = {};
    uint32_t nbar = 0;
    bars[nbar++] = (VkImageMemoryBarrier2){
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
        .srcAccessMask = VK_ACCESS_2_NONE,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = slot->layout,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = slot->img.image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    if (msaa) {
        bars[nbar++] = (VkImageMemoryBarrier2){
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            .oldLayout = slot->msaa_color_layout,
            .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = slot->msaa_color,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
        };
    }
    if (slot->depth_target.view != VK_NULL_HANDLE) {
        VkImageAspectFlags daspect = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (slot->depth_target.format == VK_FORMAT_D24_UNORM_S8_UINT) {
            daspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
        }
        bars[nbar++] = (VkImageMemoryBarrier2){
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            .dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
            .oldLayout = slot->depth_layout,
            .newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = slot->depth_target.image,
            .subresourceRange = {
                .aspectMask = daspect,
                .levelCount = 1,
                .layerCount = 1,
            },
        };
    }
    VkDependencyInfo dep1 = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = nbar,
        .pImageMemoryBarriers = bars,
    };
    vkCmdPipelineBarrier2(slot->cmd, &dep1);

    VkClearValue clear = { .color = { .float32 = { 0.02f, 0.04f, 0.08f, 1.0f } } };
    /* Modern Reversed-Z: 0.0f is the farthest possible depth; nearer objects have higher Z */
    VkClearValue depth_clear = { .depthStencil = { .depth = KHR_REVERSED_Z_CLEAR, .stencil = 0 } };
    VkRenderingAttachmentInfo color_att = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = msaa ? slot->msaa_color_view : slot->view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .resolveMode = msaa ? VK_RESOLVE_MODE_AVERAGE_BIT : VK_RESOLVE_MODE_NONE,
        .resolveImageView = msaa ? slot->view : VK_NULL_HANDLE,
        .resolveImageLayout = msaa ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                   : VK_IMAGE_LAYOUT_UNDEFINED,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = msaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                        : VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = clear,
    };
    VkRenderingAttachmentInfo depth_att = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = slot->depth_target.view,
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .clearValue = depth_clear,
    };
    VkRenderingInfo ri = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = { .offset = { 0, 0 }, .extent = { w, h } },
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_att,
        .pDepthAttachment = slot->depth_target.view != VK_NULL_HANDLE ? &depth_att : nullptr,
    };
    vkCmdBeginRendering(slot->cmd, &ri);
    VkViewport vp = {
        .x = 0.0f, .y = 0.0f,
        .width = (float)w, .height = (float)h,
        .minDepth = 0.0f, .maxDepth = 1.0f,
    };
    vkCmdSetViewport(slot->cmd, 0, 1, &vp);
    VkRect2D sc = { .offset = { 0, 0 }, .extent = { w, h } };
    vkCmdSetScissor(slot->cmd, 0, 1, &sc);
    vkCmdBindPipeline(slot->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, slot->pipe.pipeline);
    if (d->has_maintenance6 && slot->pfn_push2 != nullptr) {
        VkPushConstantsInfo pci = {
            .sType = VK_STRUCTURE_TYPE_PUSH_CONSTANTS_INFO,
            .layout = slot->pipe.layout,
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            .offset = 0,
            .size = sizeof(khr_card_push_t),
            .pValues = &push,
        };
        slot->pfn_push2(slot->cmd, &pci);
    } else {
        vkCmdPushConstants(slot->cmd, slot->pipe.layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(khr_card_push_t), &push);
    }
    vkCmdDraw(slot->cmd, 6, card_count, 0, 0);
    bool draw_gpu_scene = gpu_scene != nullptr && gpu_scene->inst_pipe != nullptr &&
                          gpu_scene->inst_pipe->pipeline != VK_NULL_HANDLE &&
                          gpu_scene->indirect_cmd_buffer != VK_NULL_HANDLE &&
                          gpu_scene->inst_push != nullptr;
    bool draw_mesh = !draw_gpu_scene && mesh != nullptr && mesh->pipeline != VK_NULL_HANDLE &&
                     mesh_push != nullptr && mesh_push->index_count >= 3U &&
                     mesh_push->verts_addr != 0 && mesh_push->indices_addr != 0;
    bool draw_plot = !draw_gpu_scene && !draw_mesh && plot != nullptr &&
                     plot->pipeline != VK_NULL_HANDLE &&
                     plot_push != nullptr && plot_push->count >= 2U &&
                     plot_push->samples_addr != 0;
    if (draw_gpu_scene || draw_mesh || draw_plot) {
        uint32_t top = plot_top_px;
        if (top >= h) {
            top = 0;
        }
        uint32_t ph = h - top;
        if (ph < 1U) {
            ph = 1;
        }
        VkViewport pvp = {
            .x = 0.0f,
            .y = (float)top,
            .width = (float)w,
            .height = (float)ph,
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        vkCmdSetViewport(slot->cmd, 0, 1, &pvp);
        VkRect2D psc = {
            .offset = { 0, (int32_t)top },
            .extent = { w, ph },
        };
        vkCmdSetScissor(slot->cmd, 0, 1, &psc);
        if (draw_gpu_scene) {
            if (gpu_scene->grid_pipe != nullptr && gpu_scene->grid_push != nullptr) {
                khr_grid_pipeline_draw(gpu_scene->grid_pipe, slot->cmd, gpu_scene->grid_push);
            }
            if (gpu_scene->descriptor_heap != nullptr) {
                khr_descriptor_heap_bind(gpu_scene->descriptor_heap, slot->cmd,
                                         VK_PIPELINE_BIND_POINT_GRAPHICS,
                                         gpu_scene->inst_pipe->layout);
            }
            if (gpu_scene->mesh_pass_count > 0) {
                for (uint32_t m = 0; m < gpu_scene->mesh_pass_count; m++) {
                    khr_mesh_instanced_draw_indirect(gpu_scene->inst_pipe, slot->cmd,
                                                     &gpu_scene->mesh_pushes[m],
                                                     gpu_scene->indirect_cmd_buffer,
                                                     gpu_scene->indirect_cmd_offsets[m],
                                                     gpu_scene->draw_counts[m],
                                                     sizeof(khr_draw_indirect_cmd_t));
                }
            } else {
                khr_mesh_instanced_draw_indirect(gpu_scene->inst_pipe, slot->cmd,
                                                 gpu_scene->inst_push,
                                                 gpu_scene->indirect_cmd_buffer,
                                                 gpu_scene->indirect_cmd_offset,
                                                 gpu_scene->draw_count,
                                                 sizeof(khr_draw_indirect_cmd_t));
            }
        } else if (draw_mesh) {
            khr_mesh_draw(mesh, slot->cmd, mesh_push);
        } else {
            khr_plot_draw(plot, slot->cmd, plot_push, plot_push->count - 1U);
        }
    }
    bool draw_gizmo = gizmo != nullptr && mesh != nullptr &&
                      mesh->pipeline != VK_NULL_HANDLE &&
                      gizmo->s >= 8U && gizmo->index_count >= 3U &&
                      gizmo->verts_addr != 0 && gizmo->indices_addr != 0;
    if (draw_gizmo) {
        uint32_t gx = gizmo->x;
        uint32_t gy = gizmo->y;
        uint32_t gs = gizmo->s;
        if (gx < w && gy < h && gx + gs <= w && gy + gs <= h) {
            if (slot->depth_target.view != VK_NULL_HANDLE) {
                VkImageAspectFlags daspect = VK_IMAGE_ASPECT_DEPTH_BIT;
                if (slot->depth_target.format == VK_FORMAT_D24_UNORM_S8_UINT) {
                    daspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
                }
                VkClearAttachment ca = {
                    .aspectMask = daspect,
                    .clearValue = depth_clear,
                };
                VkClearRect cr = {
                    .rect = {
                        .offset = { (int32_t)gx, (int32_t)gy },
                        .extent = { gs, gs },
                    },
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                };
                vkCmdClearAttachments(slot->cmd, 1, &ca, 1, &cr);
            }
            VkViewport gvp = {
                .x = (float)gx,
                .y = (float)gy,
                .width = (float)gs,
                .height = (float)gs,
                .minDepth = 0.0f,
                .maxDepth = 1.0f,
            };
            vkCmdSetViewport(slot->cmd, 0, 1, &gvp);
            VkRect2D gsc = {
                .offset = { (int32_t)gx, (int32_t)gy },
                .extent = { gs, gs },
            };
            vkCmdSetScissor(slot->cmd, 0, 1, &gsc);
            khr_mesh_push_t gp = {
                .verts_addr = gizmo->verts_addr,
                .indices_addr = gizmo->indices_addr,
                .index_count = gizmo->index_count,
                .vert_count = gizmo->vert_count,
            };
            const uint32_t tint[3] = {
                232U | (56U << 8) | (56U << 16),
                56U | (196U << 8) | (72U << 16),
                56U | (120U << 8) | (232U << 16),
            };
            for (int a = 0; a < 3; a++) {
                khr_gizmo_arm_apply(&gp, gizmo->r0, gizmo->r1, gizmo->r2, a,
                                    tint[a]);
                khr_mesh_draw(mesh, slot->cmd, &gp);
            }
        }
    }
    vkCmdEndRendering(slot->cmd);
    if (msaa) {
        slot->msaa_color_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    if (slot->depth_target.view != VK_NULL_HANDLE) {
        slot->depth_layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    }

    /* Export layout: the compositor reads dma-buf memory directly (LINEAR),
     * so end in GENERAL, never in a WSI present layout. */
    VkImageMemoryBarrier2 to_export = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_NONE,
        .dstAccessMask = VK_ACCESS_2_NONE,
        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = slot->img.image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    VkDependencyInfo dep2 = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &to_export,
    };
    vkCmdPipelineBarrier2(slot->cmd, &dep2);

    if (slot->has_query_pool) {
        vkCmdWriteTimestamp(slot->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot->query_pool, 1);
    }

    if (vkEndCommandBuffer(slot->cmd) != VK_SUCCESS) {
        return false;
    }
    VkCommandBufferSubmitInfo cb_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = slot->cmd,
    };
    VkSemaphoreSubmitInfo sig_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = signal_sem,
        .value = signal_value,
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
    VkSubmitInfo2 submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &cb_info,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos = &sig_info,
    };
    /* Externally synchronized queue (shared handle on UMA): serialize here;
     * still no host wait — submit returns once queued. */
    khr_gfx_device_lock_queues(d);
    VkResult sr = vkQueueSubmit2(d->gfx_queue, 1, &submit, VK_NULL_HANDLE);
    khr_gfx_device_unlock_queues(d);
    if (sr != VK_SUCCESS) {
        return false;
    }
    slot->layout = VK_IMAGE_LAYOUT_GENERAL;
    slot->painted++;
    return true;
}

[[nodiscard]]
bool khr_dmabuf_slot_render(khr_gfx_device_t* d, khr_dmabuf_slot_t* slot,
                            khr_bda_arena_t* arena, uint64_t frame_no,
                            VkSemaphore signal_sem, uint64_t signal_value) {
    if (arena == nullptr) {
        return false;
    }
    uint32_t w = (slot != nullptr) ? slot->img.w : 0;
    uint32_t h = (slot != nullptr) ? slot->img.h : 0;
    khr_card_instance_t* inst = nullptr;
    VkDeviceAddress inst_addr = 0;
    if (!khr_bda_arena_alloc(arena, sizeof(khr_card_instance_t), 16,
                             (void**)&inst, &inst_addr)) {
        return false;
    }
    uint32_t phase = (uint32_t)(frame_no % 3U);
    uint32_t r = (phase == 0U) ? 255U : 20U;
    uint32_t g = (phase == 1U) ? 255U : 20U;
    uint32_t b = (phase == 2U) ? 255U : 20U;
    uint32_t rgba = r | (g << 8) | (b << 16) | (255U << 24);
    *inst = (khr_card_instance_t){
        .rect = { 0.0f, 0.0f, (float)w, (float)h },
        .bg_rgba = rgba,
        .border_rgba = rgba,
        .corner_radius = 0.0f,
        .border_width = 0.0f,
    };
    return khr_dmabuf_slot_render_cards(d, slot, inst_addr, 1, signal_sem,
                                        signal_value);
}

[[nodiscard]]
uint64_t khr_dmabuf_slot_query_gpu_time_ns(khr_gfx_device_t* d, khr_dmabuf_slot_t* slot) {
    if (d == nullptr || d->device == VK_NULL_HANDLE || slot == nullptr || !slot->has_query_pool) {
        return 0;
    }
    uint64_t ts[2] = {};
    VkResult res = vkGetQueryPoolResults(d->device, slot->query_pool, 0, 2,
                                         sizeof(ts), ts, sizeof(uint64_t),
                                         VK_QUERY_RESULT_64_BIT);
    if (res == VK_SUCCESS && ts[1] >= ts[0]) {
        double diff = (double)(ts[1] - ts[0]);
        slot->gpu_time_ns = (uint64_t)(diff * (double)d->timestamp_period);
        return slot->gpu_time_ns;
    }
    return 0;
}

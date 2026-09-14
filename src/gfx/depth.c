#include "khoros/gfx/depth.h"

[[nodiscard]]
static uint32_t khr_find_depth_memory_type(VkPhysicalDevice phy,
                                           uint32_t type_filter,
                                           VkMemoryPropertyFlags req_flags,
                                           bool* out_lazy) {
    VkPhysicalDeviceMemoryProperties mem_props = {};
    vkGetPhysicalDeviceMemoryProperties(phy, &mem_props);
    if (out_lazy != nullptr) {
        *out_lazy = false;
    }

    /* Priority 1: Check for lazily allocated device-local memory (optimal for tile/L3 cache) */
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((type_filter & (1U << i)) &&
            (mem_props.memoryTypes[i].propertyFlags &
             (req_flags | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)) ==
            (req_flags | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)) {
            if (out_lazy != nullptr) {
                *out_lazy = true;
            }
            return i;
        }
    }

    /* Priority 2: Standard device-local memory */
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((type_filter & (1U << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & req_flags) == req_flags) {
            return i;
        }
    }

    /* Priority 3: Any memory type satisfying the filter */
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if (type_filter & (1U << i)) {
            return i;
        }
    }

    return UINT32_MAX;
}

[[nodiscard]]
bool khr_depth_target_create(const khr_gfx_device_t *d,
                             khr_depth_target_t *depth,
                             uint32_t width,
                             uint32_t height,
                             VkSampleCountFlagBits samples,
                             VkFormat format) {
    if (d == nullptr || d->device == VK_NULL_HANDLE || depth == nullptr ||
        width == 0 || height == 0) {
        return false;
    }

    if (format == VK_FORMAT_UNDEFINED) {
        format = khr_gfx_depth_format(d);
        if (format == VK_FORMAT_UNDEFINED) {
            return false;
        }
    }

    if (samples == 0) {
        samples = VK_SAMPLE_COUNT_1_BIT;
    }

    *depth = (khr_depth_target_t){};
    depth->width = width;
    depth->height = height;
    depth->samples = samples;
    depth->format = format;

    /* 1. Create transient depth image */
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { .width = width, .height = height, .depth = 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = samples,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    if (vkCreateImage(d->device, &ici, nullptr, &depth->image) != VK_SUCCESS) {
        return false;
    }

    /* 2. Query requirements and allocate memory (probe for lazily allocated memory) */
    VkMemoryRequirements mem_reqs = {};
    vkGetImageMemoryRequirements(d->device, depth->image, &mem_reqs);

    bool is_lazy = false;
    uint32_t mem_type_idx = khr_find_depth_memory_type(
        d->phy,
        mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        &is_lazy
    );

    if (mem_type_idx == UINT32_MAX) {
        vkDestroyImage(d->device, depth->image, nullptr);
        depth->image = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_reqs.size,
        .memoryTypeIndex = mem_type_idx,
    };

    if (vkAllocateMemory(d->device, &ai, nullptr, &depth->memory) != VK_SUCCESS) {
        vkDestroyImage(d->device, depth->image, nullptr);
        depth->image = VK_NULL_HANDLE;
        return false;
    }

    if (vkBindImageMemory(d->device, depth->image, depth->memory, 0) != VK_SUCCESS) {
        khr_depth_target_destroy(d, depth);
        return false;
    }
    depth->is_lazy = is_lazy;

    /* 3. Create depth image view with correct aspect mask */
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (format == VK_FORMAT_D24_UNORM_S8_UINT ||
        format == VK_FORMAT_D32_SFLOAT_S8_UINT) {
        aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    }

    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = depth->image,
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

    if (vkCreateImageView(d->device, &vci, nullptr, &depth->view) != VK_SUCCESS) {
        khr_depth_target_destroy(d, depth);
        return false;
    }

    return true;
}

void khr_depth_target_destroy(const khr_gfx_device_t *d,
                              khr_depth_target_t *depth) {
    if (d == nullptr || depth == nullptr) {
        return;
    }

    VkDevice dev = d->device;
    if (dev == VK_NULL_HANDLE) {
        *depth = (khr_depth_target_t){};
        return;
    }

    if (depth->view != VK_NULL_HANDLE) {
        vkDestroyImageView(dev, depth->view, nullptr);
    }
    if (depth->memory != VK_NULL_HANDLE) {
        vkFreeMemory(dev, depth->memory, nullptr);
    }
    if (depth->image != VK_NULL_HANDLE) {
        vkDestroyImage(dev, depth->image, nullptr);
    }

    *depth = (khr_depth_target_t){};
}
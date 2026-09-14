#ifndef KHOROS_GFX_DEPTH_H
#define KHOROS_GFX_DEPTH_H

#if __STDC_VERSION__ < 202311L
#error "Khoros requires pure ISO C23 (compile with -std=c23)"
#endif

#ifdef _STDBOOL_H
#error "<stdbool.h> is strictly banned. In C23, bool, true, and false are native keywords."
#endif

#ifdef _STDALIGN_H
#error "<stdalign.h> is strictly banned. In C23, alignas and alignof are native keywords."
#endif

#include "khoros/gfx/device.h"
#include <stdint.h>
#include <vulkan/vulkan.h>

constexpr VkFormat KHR_DEPTH_FORMAT_PRIMARY = VK_FORMAT_D32_SFLOAT;
constexpr VkFormat KHR_DEPTH_FORMAT_FALLBACK = VK_FORMAT_D24_UNORM_S8_UINT;

/* Reversed-Z: 0.0f is the farthest possible numerical depth */
constexpr float KHR_REVERSED_Z_CLEAR = 0.0f;
constexpr VkCompareOp KHR_REVERSED_Z_COMPARE_OP = VK_COMPARE_OP_GREATER_OR_EQUAL;

typedef struct khr_depth_target {
    VkImage               image;
    VkDeviceMemory        memory;
    VkImageView           view;
    uint32_t              width;
    uint32_t              height;
    VkSampleCountFlagBits samples;
    VkFormat              format;
    bool                  is_lazy;
} khr_depth_target_t;

[[nodiscard]]
bool khr_depth_target_create(const khr_gfx_device_t* d,
                             khr_depth_target_t* depth,
                             uint32_t width,
                             uint32_t height,
                             VkSampleCountFlagBits samples,
                             VkFormat format);

void khr_depth_target_destroy(const khr_gfx_device_t* d,
                              khr_depth_target_t* depth);

#endif /* KHOROS_GFX_DEPTH_H */
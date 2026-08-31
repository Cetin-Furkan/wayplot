#ifndef RENDERER_PASS_H
#define RENDERER_PASS_H

#include "vulkan/device.h"

#include <stdint.h>
#include <vulkan/vulkan.h>

#ifndef WP_SWAPCHAIN_IMAGES
#define WP_SWAPCHAIN_IMAGES 3
#endif

/*
 * Owns BeginRendering / EndRendering and depth. Draws (lit, text) only
 * bind and DrawIndexed inside a begin/end. See docs/PASSES.md and docs/HOST.md.
 */

struct wp_pass {
    struct wp_device *dev;
    VkFormat color_fmt;
    VkImage depth[WP_SWAPCHAIN_IMAGES];
    VkImageView depth_view[WP_SWAPCHAIN_IMAGES];
    VkDeviceMemory depth_mem[WP_SWAPCHAIN_IMAGES];
    uint32_t width;
    uint32_t height;
    float clear[4];
};

[[nodiscard]] int wp_pass_init(struct wp_pass *p, struct wp_device *d, VkFormat color_fmt,
                               uint32_t width, uint32_t height);
/* Replaces depth images. Waits GPU idle. Safe to call while a cmd is
 * being recorded if that cmd has not been submitted. */
[[nodiscard]] int wp_pass_resize(struct wp_pass *p, uint32_t width, uint32_t height);
void wp_pass_opaque_begin(struct wp_pass *p, VkCommandBuffer cmd, VkImageView color_view,
                          uint32_t width, uint32_t height, uint32_t slot);
void wp_pass_opaque_end(VkCommandBuffer cmd);
void wp_pass_overlay_begin(struct wp_pass *p, VkCommandBuffer cmd, VkImageView color_view,
                           uint32_t width, uint32_t height);
void wp_pass_overlay_end(VkCommandBuffer cmd);
void wp_pass_destroy(struct wp_pass *p);

#endif /* RENDERER_PASS_H */
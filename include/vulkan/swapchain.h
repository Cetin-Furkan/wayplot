#ifndef VULKAN_SWAPCHAIN_H
#define VULKAN_SWAPCHAIN_H

#include "vulkan/device.h"
#include "vulkan/negotiate.h"

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#define WP_SWAPCHAIN_IMAGES 3

struct wp_plane {
    uint32_t stride;
    uint32_t offset;
};

struct wp_vk_image {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkCommandBuffer cmd;
    int dma_fd;
    uint32_t plane_count;
    struct wp_plane planes[4];
    uint64_t modifier;
    uint64_t last_acquire;
    uint64_t last_release;
    VkSemaphore release_sem;
    int release_fd;
    uint32_t drm_handle;
};

struct wp_swapchain {
    struct wp_vk_image images[WP_SWAPCHAIN_IMAGES];
    struct wp_vk_image *retired;
    uint32_t retired_count;
    uint32_t retired_cap;
    struct wp_negotiated params;
    uint32_t width;
    uint32_t height;
    bool allocated;
    uint32_t cursor;
};

void wp_swapchain_init(struct wp_swapchain *sc);
void wp_swapchain_free_retired(struct wp_device *d, struct wp_swapchain *sc);
void wp_swapchain_destroy_image(struct wp_device *d, struct wp_vk_image *img);

[[nodiscard]] int wp_swapchain_create(struct wp_device *d, struct wp_swapchain *sc,
                                      uint32_t width, uint32_t height,
                                      const struct wp_negotiated *np);
void wp_swapchain_retire(struct wp_swapchain *sc);
[[nodiscard]] int wp_swapchain_pick(struct wp_device *d, struct wp_swapchain *sc, uint32_t *out);
[[nodiscard]] int wp_swapchain_wait_gpu(struct wp_device *d, const struct wp_vk_image *img);

#endif /* VULKAN_SWAPCHAIN_H */

#ifndef VULKAN_GPU_H
#define VULKAN_GPU_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <vulkan/vulkan.h>

/* Physical-device dump + pick. No VkDevice, no Wayland object ids.
 * Input is compositor zwp_linux_dmabuf_feedback_v1.main_device (dev_t). */

struct wp_gpu_info {
    VkPhysicalDevice phy;
    char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    uint32_t api_version;
    uint32_t vendor_id;
    uint32_t device_id;
    uint32_t driver_version;
    bool api_1_4;
    bool phy_drm;
    bool has_render;
    bool has_primary;
    dev_t render;
    dev_t primary;
    bool dma_buf;
    bool drm_modifier;
    bool memory_fd;
    bool semaphore_fd;
    bool push_descriptor;
    bool host_image_copy;
    bool maintenance5;
};

struct wp_gpu_caps {
    VkInstance instance;
    VkPhysicalDevice picked;
    int picked_index;
    struct wp_gpu_info *v;
    uint32_t n;
};

[[nodiscard]] int wp_gpu_enumerate(struct wp_gpu_caps *c);
[[nodiscard]] int wp_gpu_pick(struct wp_gpu_caps *c, dev_t compositor_dev);
void wp_gpu_caps_print(const struct wp_gpu_caps *c, FILE *out);
void wp_gpu_caps_free(struct wp_gpu_caps *c);

#endif /* VULKAN_GPU_H */

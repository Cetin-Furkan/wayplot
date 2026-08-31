#ifndef VULKAN_DEVICE_H
#define VULKAN_DEVICE_H

#include "vulkan/gpu.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <vulkan/vulkan.h>

struct wp_device {
    VkInstance instance;
    VkPhysicalDevice phy;
    VkDevice device;
    VkQueue gfx;
    VkQueue xfer;
    uint32_t gfx_family;
    uint32_t xfer_family;
    VkCommandPool pool;
    VkDescriptorSetLayout push_set;
    VkPipelineLayout push_layout;
    uint32_t max_push_descriptors;

    VkSemaphore acquire_sem;
    int acquire_fd;
    uint64_t acquire_point;

    int drm_fd;
    dev_t render;
    dev_t primary;
    bool host_image_copy;
    bool push_descriptor;
    bool maintenance5;

    PFN_vkGetMemoryFdKHR vkGetMemoryFdKHR;
    PFN_vkGetSemaphoreFdKHR vkGetSemaphoreFdKHR;
    PFN_vkGetImageDrmFormatModifierPropertiesEXT vkGetImageDrmFormatModifierPropertiesEXT;
};

[[nodiscard]] int wp_device_open(struct wp_device *d, dev_t compositor_dev);
void wp_device_close(struct wp_device *d);
void wp_device_print(const struct wp_device *d, FILE *out);

[[nodiscard]] int wp_device_prove_host_copy(struct wp_device *d);
[[nodiscard]] uint32_t wp_find_memory_type(VkPhysicalDeviceMemoryProperties *props,
                                           uint32_t type_bits, VkMemoryPropertyFlags prefer);

[[nodiscard]] int wp_timeline_create(struct wp_device *d, VkSemaphore *sem, int *fd);
void wp_timeline_destroy(struct wp_device *d, VkSemaphore *sem, int *fd);

#endif /* VULKAN_DEVICE_H */

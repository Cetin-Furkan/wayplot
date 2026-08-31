#ifndef VULKAN_BUFFER_H
#define VULKAN_BUFFER_H

#include "vulkan/device.h"

#include <stddef.h>
#include <vulkan/vulkan.h>

struct wp_buffer {
    VkBuffer buf;
    VkDeviceMemory mem;
    VkDeviceSize size;
    void *mapped;
};

[[nodiscard]] int wp_buffer_create(struct wp_device *d, VkDeviceSize size, VkBufferUsageFlags usage,
                                   VkMemoryPropertyFlags prefer, struct wp_buffer *out);
[[nodiscard]] int wp_buffer_upload(struct wp_device *d, struct wp_buffer *b, const void *data, VkDeviceSize size);
void wp_buffer_destroy(struct wp_device *d, struct wp_buffer *b);

#endif /* VULKAN_BUFFER_H */

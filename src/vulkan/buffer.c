#define _GNU_SOURCE
#include "vulkan/buffer.h"

#include <errno.h>
#include <string.h>

int wp_buffer_create(struct wp_device *d, VkDeviceSize size, VkBufferUsageFlags usage,
                     VkMemoryPropertyFlags prefer, struct wp_buffer *out)
{
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkDeviceBufferMemoryRequirements q = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS,
        .pCreateInfo = &bci,
    };
    VkMemoryRequirements2 memreq = { .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
    VkPhysicalDeviceMemoryProperties mprops;
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    uint32_t idx;
    VkMemoryPropertyFlags flags;

    if (!d || !out || size == 0)
        return -EINVAL;
    memset(out, 0, sizeof(*out));

    vkGetDeviceBufferMemoryRequirements(d->device, &q, &memreq);
    if (vkCreateBuffer(d->device, &bci, NULL, &out->buf) != VK_SUCCESS)
        return -EIO;

    vkGetPhysicalDeviceMemoryProperties(d->phy, &mprops);
    idx = wp_find_memory_type(&mprops, memreq.memoryRequirements.memoryTypeBits, prefer);
    if (idx == UINT32_MAX)
        idx = wp_find_memory_type(&mprops, memreq.memoryRequirements.memoryTypeBits, 0);
    if (idx == UINT32_MAX) {
        wp_buffer_destroy(d, out);
        return -ENOMEM;
    }
    ai.allocationSize = memreq.memoryRequirements.size;
    ai.memoryTypeIndex = idx;
    if (vkAllocateMemory(d->device, &ai, NULL, &out->mem) != VK_SUCCESS) {
        idx = wp_find_memory_type(&mprops, memreq.memoryRequirements.memoryTypeBits, 0);
        if (idx == UINT32_MAX) {
            wp_buffer_destroy(d, out);
            return -ENOMEM;
        }
        ai.memoryTypeIndex = idx;
        if (vkAllocateMemory(d->device, &ai, NULL, &out->mem) != VK_SUCCESS) {
            wp_buffer_destroy(d, out);
            return -ENOMEM;
        }
    }
    if (vkBindBufferMemory(d->device, out->buf, out->mem, 0) != VK_SUCCESS) {
        wp_buffer_destroy(d, out);
        return -EIO;
    }
    out->size = size;
    flags = mprops.memoryTypes[ai.memoryTypeIndex].propertyFlags;
    if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        if (vkMapMemory(d->device, out->mem, 0, size, 0, &out->mapped) != VK_SUCCESS)
            out->mapped = NULL;
    }
    return 0;
}

int wp_buffer_upload(struct wp_device *d, struct wp_buffer *b, const void *data, VkDeviceSize size)
{
    struct wp_buffer staging = { 0 };
    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo cai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VkBufferCopy copy = { .size = size };
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
    int ret;

    if (!d || !b || !data || size == 0 || size > b->size)
        return -EINVAL;
    if (b->mapped) {
        memcpy(b->mapped, data, (size_t)size);
        return 0;
    }

    ret = wp_buffer_create(d, size,
                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           &staging);
    if (ret < 0)
        return ret;
    if (!staging.mapped) {
        wp_buffer_destroy(d, &staging);
        return -ENOMEM;
    }
    memcpy(staging.mapped, data, (size_t)size);

    cai.commandPool = d->pool;
    if (vkAllocateCommandBuffers(d->device, &cai, &cmd) != VK_SUCCESS) {
        wp_buffer_destroy(d, &staging);
        return -EIO;
    }
    vkBeginCommandBuffer(cmd, &bi);
    vkCmdCopyBuffer(cmd, staging.buf, b->buf, 1, &copy);
    vkEndCommandBuffer(cmd);
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    ret = 0;
    if (vkQueueSubmit(d->xfer, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS)
        ret = -EIO;
    else if (vkQueueWaitIdle(d->xfer) != VK_SUCCESS)
        ret = -EIO;
    vkFreeCommandBuffers(d->device, d->pool, 1, &cmd);
    wp_buffer_destroy(d, &staging);
    return ret;
}

void wp_buffer_destroy(struct wp_device *d, struct wp_buffer *b)
{
    if (!d || !b)
        return;
    if (b->mapped && b->mem)
        vkUnmapMemory(d->device, b->mem);
    if (b->buf)
        vkDestroyBuffer(d->device, b->buf, NULL);
    if (b->mem)
        vkFreeMemory(d->device, b->mem, NULL);
    memset(b, 0, sizeof(*b));
}

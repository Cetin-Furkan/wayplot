#define _GNU_SOURCE
#include "renderer/pass.h"

#include "vulkan/device.h"

#include <errno.h>
#include <string.h>

static void destroy_depth(struct wp_pass *p)
{
    uint32_t i;
    if (!p || !p->dev)
        return;
    for (i = 0; i < WP_SWAPCHAIN_IMAGES; i++) {
        if (p->depth_view[i])
            vkDestroyImageView(p->dev->device, p->depth_view[i], NULL);
        if (p->depth[i])
            vkDestroyImage(p->dev->device, p->depth[i], NULL);
        if (p->depth_mem[i])
            vkFreeMemory(p->dev->device, p->depth_mem[i], NULL);
        p->depth_view[i] = VK_NULL_HANDLE;
        p->depth[i] = VK_NULL_HANDLE;
        p->depth_mem[i] = VK_NULL_HANDLE;
    }
}

static int make_depth(struct wp_pass *p, uint32_t w, uint32_t h)
{
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_D32_SFLOAT,
        .extent = { w, h, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkDeviceImageMemoryRequirements q = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS,
        .pCreateInfo = &ici,
    };
    VkMemoryRequirements2 memreq = { .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
    VkPhysicalDeviceMemoryProperties mprops;
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    uint32_t i, idx;

    destroy_depth(p);
    vkGetDeviceImageMemoryRequirements(p->dev->device, &q, &memreq);
    vkGetPhysicalDeviceMemoryProperties(p->dev->phy, &mprops);
    idx = wp_find_memory_type(&mprops, memreq.memoryRequirements.memoryTypeBits,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (idx == UINT32_MAX)
        idx = wp_find_memory_type(&mprops, memreq.memoryRequirements.memoryTypeBits, 0);
    if (idx == UINT32_MAX)
        return -ENOMEM;
    ai.allocationSize = memreq.memoryRequirements.size;
    ai.memoryTypeIndex = idx;

    for (i = 0; i < WP_SWAPCHAIN_IMAGES; i++) {
        VkImageViewCreateInfo vci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_D32_SFLOAT,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
        };
        if (vkCreateImage(p->dev->device, &ici, NULL, &p->depth[i]) != VK_SUCCESS)
            return -EIO;
        if (vkAllocateMemory(p->dev->device, &ai, NULL, &p->depth_mem[i]) != VK_SUCCESS)
            return -ENOMEM;
        if (vkBindImageMemory(p->dev->device, p->depth[i], p->depth_mem[i], 0) != VK_SUCCESS)
            return -EIO;
        vci.image = p->depth[i];
        if (vkCreateImageView(p->dev->device, &vci, NULL, &p->depth_view[i]) != VK_SUCCESS)
            return -EIO;
    }
    p->width = w;
    p->height = h;
    return 0;
}

int wp_pass_init(struct wp_pass *p, struct wp_device *d, VkFormat color_fmt,
                 uint32_t width, uint32_t height)
{
    if (!p || !d || width == 0 || height == 0)
        return -EINVAL;
    memset(p, 0, sizeof(*p));
    p->dev = d;
    p->color_fmt = color_fmt;
    p->clear[0] = 0.07f;
    p->clear[1] = 0.16f;
    p->clear[2] = 0.22f;
    p->clear[3] = 1.0f;
    return make_depth(p, width, height);
}

int wp_pass_resize(struct wp_pass *p, uint32_t width, uint32_t height)
{
    if (!p || !p->dev || width == 0 || height == 0)
        return -EINVAL;
    if (p->width == width && p->height == height)
        return 0;
    vkDeviceWaitIdle(p->dev->device);
    return make_depth(p, width, height);
}

static void set_full_viewport(VkCommandBuffer cmd, uint32_t width, uint32_t height)
{
    VkViewport viewport = {
        .width = (float)width,
        .height = (float)height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    VkRect2D scissor = { .extent = { width, height } };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

void wp_pass_opaque_begin(struct wp_pass *p, VkCommandBuffer cmd, VkImageView color_view,
                          uint32_t width, uint32_t height, uint32_t slot)
{
    VkImageMemoryBarrier2 db = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
        .dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
        .dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    VkDependencyInfo dep = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &db,
    };
    VkRenderingAttachmentInfo color = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    };
    VkRenderingAttachmentInfo depth = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .clearValue.depthStencil.depth = 1.0f,
    };
    VkRenderingInfo ri = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color,
        .pDepthAttachment = &depth,
    };

    if (!p || !cmd || !color_view || slot >= WP_SWAPCHAIN_IMAGES)
        return;
    if (width == 0 || height == 0)
        return;
    if (p->width != width || p->height != height) {
        if (wp_pass_resize(p, width, height) < 0)
            return;
    }

    memcpy(color.clearValue.color.float32, p->clear, sizeof(p->clear));
    db.image = p->depth[slot];
    vkCmdPipelineBarrier2(cmd, &dep);
    color.imageView = color_view;
    depth.imageView = p->depth_view[slot];
    ri.renderArea.extent = (VkExtent2D){ width, height };
    vkCmdBeginRendering(cmd, &ri);
    set_full_viewport(cmd, width, height);
}

void wp_pass_opaque_end(VkCommandBuffer cmd)
{
    if (cmd)
        vkCmdEndRendering(cmd);
}

void wp_pass_overlay_begin(struct wp_pass *p, VkCommandBuffer cmd, VkImageView color_view,
                           uint32_t width, uint32_t height)
{
    VkMemoryBarrier2 mb = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
    };
    VkDependencyInfo dep = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &mb,
    };
    VkRenderingAttachmentInfo color = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    };
    VkRenderingInfo ri = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color,
    };

    if (!p || !cmd || !color_view || width == 0 || height == 0)
        return;
    vkCmdPipelineBarrier2(cmd, &dep);
    color.imageView = color_view;
    ri.renderArea.extent = (VkExtent2D){ width, height };
    vkCmdBeginRendering(cmd, &ri);
    set_full_viewport(cmd, width, height);
}

void wp_pass_overlay_end(VkCommandBuffer cmd)
{
    if (cmd)
        vkCmdEndRendering(cmd);
}

void wp_pass_destroy(struct wp_pass *p)
{
    if (!p || !p->dev)
        return;
    vkDeviceWaitIdle(p->dev->device);
    destroy_depth(p);
    memset(p, 0, sizeof(*p));
}

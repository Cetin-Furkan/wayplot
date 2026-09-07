#include "khoros/gfx/frame.h"

#include <string.h>

constexpr VkFormat KHR_FRAME_FORMAT = VK_FORMAT_B8G8R8A8_UNORM;
constexpr uint64_t KHR_FRAME_WAIT_NS = 5'000'000'000ULL; /* 5 s host wait */

[[nodiscard]]
static uint32_t khr_frame_mem_type(VkPhysicalDevice phy, uint32_t filter,
                                   VkMemoryPropertyFlags req, bool want_device_local) {
    VkPhysicalDeviceMemoryProperties props = {};
    vkGetPhysicalDeviceMemoryProperties(phy, &props);
    if (want_device_local) {
        for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
            VkMemoryPropertyFlags f = props.memoryTypes[i].propertyFlags;
            if ((filter & (1U << i)) &&
                (f & (req | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) ==
                (req | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                return i;
            }
        }
    }
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if ((filter & (1U << i)) &&
            (props.memoryTypes[i].propertyFlags & req) == req) {
            return i;
        }
    }
    return UINT32_MAX;
}

[[nodiscard]]
bool khr_gfx_frame_init(khr_gfx_device_t* d, khr_gfx_frame_t* f,
                        uint32_t w, uint32_t h) {
    if (d == nullptr || d->device == VK_NULL_HANDLE || f == nullptr) {
        return false;
    }
    if (w == 0 || h == 0 || w > 4'096 || h > 4'096) {
        return false;
    }
    *f = (khr_gfx_frame_t){ .w = w, .h = h };
    f->pfn_push2 = (PFN_vkCmdPushConstants2KHR)
        vkGetDeviceProcAddr(d->device, "vkCmdPushConstants2KHR");
    f->pfn_host_copy = (PFN_vkCopyImageToMemoryEXT)
        vkGetDeviceProcAddr(d->device, "vkCopyImageToMemoryEXT");
    /* Host copy needs both the enabled feature and a resolvable entry point;
     * otherwise the transfer engine does the readback. No silent path: the
     * frame test reports which path executed. */
    f->use_host_copy = d->has_host_image_copy && f->pfn_host_copy != nullptr;

    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = KHR_FRAME_FORMAT,
        .extent = { .width = w, .height = h, .depth = 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(d->device, &ici, nullptr, &f->image) != VK_SUCCESS) {
        khr_gfx_frame_destroy(d, f);
        return false;
    }
    VkMemoryRequirements img_req = {};
    vkGetImageMemoryRequirements(d->device, f->image, &img_req);
    uint32_t img_idx = khr_frame_mem_type(d->phy, img_req.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true);
    if (img_idx == UINT32_MAX) {
        img_idx = khr_frame_mem_type(d->phy, img_req.memoryTypeBits, 0, false);
    }
    if (img_idx == UINT32_MAX) {
        khr_gfx_frame_destroy(d, f);
        return false;
    }
    VkMemoryAllocateInfo img_ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = img_req.size,
        .memoryTypeIndex = img_idx,
    };
    if (vkAllocateMemory(d->device, &img_ai, nullptr, &f->image_mem) != VK_SUCCESS) {
        khr_gfx_frame_destroy(d, f);
        return false;
    }
    if (vkBindImageMemory(d->device, f->image, f->image_mem, 0) != VK_SUCCESS) {
        khr_gfx_frame_destroy(d, f);
        return false;
    }
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = f->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = KHR_FRAME_FORMAT,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    if (vkCreateImageView(d->device, &vci, nullptr, &f->view) != VK_SUCCESS) {
        khr_gfx_frame_destroy(d, f);
        return false;
    }

    /* Readback buffer. maintenance5 usage-flags2 carries the transfer-dst
     * usage through the promoted struct when the device enabled it. */
    VkBufferUsageFlags2CreateInfo usage2 = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = d->has_maintenance5 ? &usage2 : nullptr,
        .size = (VkDeviceSize)w * h * 4,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateBuffer(d->device, &bci, nullptr, &f->readback) != VK_SUCCESS) {
        khr_gfx_frame_destroy(d, f);
        return false;
    }
    VkMemoryRequirements buf_req = {};
    vkGetBufferMemoryRequirements(d->device, f->readback, &buf_req);
    uint32_t buf_idx = khr_frame_mem_type(d->phy, buf_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        true);
    if (buf_idx == UINT32_MAX) {
        khr_gfx_frame_destroy(d, f);
        return false;
    }
    VkMemoryAllocateInfo buf_ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = buf_req.size,
        .memoryTypeIndex = buf_idx,
    };
    if (vkAllocateMemory(d->device, &buf_ai, nullptr, &f->readback_mem) != VK_SUCCESS) {
        khr_gfx_frame_destroy(d, f);
        return false;
    }
    if (vkBindBufferMemory(d->device, f->readback, f->readback_mem, 0) != VK_SUCCESS) {
        khr_gfx_frame_destroy(d, f);
        return false;
    }
    if (vkMapMemory(d->device, f->readback_mem, 0, VK_WHOLE_SIZE, 0,
                    &f->readback_host) != VK_SUCCESS) {
        khr_gfx_frame_destroy(d, f);
        return false;
    }
    f->readback_sz = (size_t)w * h * 4;

    /* Frame-local command pool: VkCommandPool is externally synchronized, so
     * sharing d->pool across threads would corrupt the driver. One pool per
     * in-flight frame keeps recording lock-free. */
    VkCommandPoolCreateInfo pool_ci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = d->gfx_family,
    };
    if (vkCreateCommandPool(d->device, &pool_ci, nullptr, &f->pool) != VK_SUCCESS) {
        khr_gfx_frame_destroy(d, f);
        return false;
    }
    VkCommandBufferAllocateInfo cb_ai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = f->pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (vkAllocateCommandBuffers(d->device, &cb_ai, &f->cmd) != VK_SUCCESS) {
        khr_gfx_frame_destroy(d, f);
        return false;
    }

    VkSemaphoreTypeCreateInfo tci = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = 0,
    };
    VkSemaphoreCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &tci,
    };
    if (vkCreateSemaphore(d->device, &sci, nullptr, &f->timeline) != VK_SUCCESS) {
        khr_gfx_frame_destroy(d, f);
        return false;
    }
    f->next_point = 1;
    return true;
}

void khr_gfx_frame_destroy(khr_gfx_device_t* d, khr_gfx_frame_t* f) {
    if (d == nullptr || f == nullptr) {
        return;
    }
    VkDevice dev = d->device;
    if (dev != VK_NULL_HANDLE) {
        if (f->timeline != VK_NULL_HANDLE) {
            vkDestroySemaphore(dev, f->timeline, nullptr);
        }
        if (f->pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(dev, f->pool, nullptr);
        }
        if (f->readback_host != nullptr && f->readback_mem != VK_NULL_HANDLE) {
            vkUnmapMemory(dev, f->readback_mem);
        }
        if (f->readback != VK_NULL_HANDLE) {
            vkDestroyBuffer(dev, f->readback, nullptr);
        }
        if (f->readback_mem != VK_NULL_HANDLE) {
            vkFreeMemory(dev, f->readback_mem, nullptr);
        }
        if (f->view != VK_NULL_HANDLE) {
            vkDestroyImageView(dev, f->view, nullptr);
        }
        if (f->image != VK_NULL_HANDLE) {
            vkDestroyImage(dev, f->image, nullptr);
        }
        if (f->image_mem != VK_NULL_HANDLE) {
            vkFreeMemory(dev, f->image_mem, nullptr);
        }
    }
    *f = (khr_gfx_frame_t){};
}

static void khr_frame_barrier(VkCommandBuffer cmd,
                              const VkImageMemoryBarrier2* img,
                              const VkMemoryBarrier2* mem) {
    VkDependencyInfo dep = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = (mem != nullptr) ? 1U : 0U,
        .pMemoryBarriers = mem,
        .imageMemoryBarrierCount = (img != nullptr) ? 1U : 0U,
        .pImageMemoryBarriers = img,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
}

[[nodiscard]]
bool khr_gfx_frame_render_red_card(khr_gfx_device_t* d, khr_gfx_frame_t* f,
                                   khr_bda_arena_t* arena, uint8_t out_bgra[4]) {
    if (d == nullptr || d->device == VK_NULL_HANDLE || f == nullptr ||
        f->cmd == VK_NULL_HANDLE || arena == nullptr || out_bgra == nullptr) {
        return false;
    }

    /* One card instance streamed through the BDA arena: fullscreen opaque red,
     * no border, zero corner radius (exact quad, no AA fringe at center). */
    khr_card_instance_t* slot = nullptr;
    VkDeviceAddress slot_addr = 0;
    if (!khr_bda_arena_alloc(arena, sizeof(khr_card_instance_t), 16,
                             (void**)&slot, &slot_addr)) {
        return false;
    }
    *slot = (khr_card_instance_t){
        .rect = { 0.0f, 0.0f, (float)f->w, (float)f->h },
        .bg_rgba = 0xFF0000FFU, /* packed RGBA: R=255 G=0 B=0 A=255 */
        .border_rgba = 0xFF0000FFU,
        .corner_radius = 0.0f,
        .border_width = 0.0f,
    };
    khr_card_push_t push = {
        .cards_addr = slot_addr,
        .screen_extent = { (float)f->w, (float)f->h },
        .scale = 1.0f,
        .card_index = 0,
    };

    khr_card_pipeline_t pipe = {};
    if (!khr_card_pipeline_init(&pipe, d, KHR_FRAME_FORMAT)) {
        return false;
    }
    bool ok = false;

    if (vkResetCommandPool(d->device, f->pool, 0) != VK_SUCCESS) {
        goto done;
    }
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (vkBeginCommandBuffer(f->cmd, &begin) != VK_SUCCESS) {
        goto done;
    }

    /* HOST_WRITE -> VERTEX_SHADER read: HOST_COHERENT only flushes CPU writes
     * to DRAM. Without this barrier the GPU L1/L2 would sample stale voxels
     * from earlier frames. */
    VkMemoryBarrier2 host_bar = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
        .srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
    };
    khr_frame_barrier(f->cmd, nullptr, &host_bar);

    VkImageMemoryBarrier2 to_attach = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
        .srcAccessMask = VK_ACCESS_2_NONE,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = f->image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    khr_frame_barrier(f->cmd, &to_attach, nullptr);

    VkClearValue clear = { .color = { .float32 = { 0.02f, 0.04f, 0.08f, 1.0f } } };
    VkRenderingAttachmentInfo color_att = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = f->view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = clear,
    };
    VkRenderingInfo ri = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = { .offset = { 0, 0 }, .extent = { f->w, f->h } },
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_att,
    };
    vkCmdBeginRendering(f->cmd, &ri);

    /* Dynamic viewport + scissor: the pipeline declares both dynamic, so the
     * VUs are undefined until set. Skipping either is VUID-vkCmdDraw-07831. */
    VkViewport vp = {
        .x = 0.0f, .y = 0.0f,
        .width = (float)f->w, .height = (float)f->h,
        .minDepth = 0.0f, .maxDepth = 1.0f,
    };
    vkCmdSetViewport(f->cmd, 0, 1, &vp);
    VkRect2D sc = { .offset = { 0, 0 }, .extent = { f->w, f->h } };
    vkCmdSetScissor(f->cmd, 0, 1, &sc);

    vkCmdBindPipeline(f->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe.pipeline);
    if (d->has_maintenance6 && f->pfn_push2 != nullptr) {
        /* maintenance6 record path: identical push through VkPushConstantsInfo. */
        VkPushConstantsInfo pci = {
            .sType = VK_STRUCTURE_TYPE_PUSH_CONSTANTS_INFO,
            .layout = pipe.layout,
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            .offset = 0,
            .size = sizeof(khr_card_push_t),
            .pValues = &push,
        };
        f->pfn_push2(f->cmd, &pci);
    } else {
        vkCmdPushConstants(f->cmd, pipe.layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(khr_card_push_t), &push);
    }
    vkCmdDraw(f->cmd, 6, 1, 0, 0);
    vkCmdEndRendering(f->cmd);

    VkImageMemoryBarrier2 to_read = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask = f->use_host_copy ? VK_PIPELINE_STAGE_2_NONE
                                         : VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .dstAccessMask = f->use_host_copy ? VK_ACCESS_2_NONE
                                          : VK_ACCESS_2_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout = f->use_host_copy ? VK_IMAGE_LAYOUT_GENERAL
                                      : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = f->image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    khr_frame_barrier(f->cmd, &to_read, nullptr);

    if (!f->use_host_copy) {
        VkBufferImageCopy region = {
            .bufferOffset = 0,
            .bufferRowLength = 0, /* tightly packed */
            .bufferImageHeight = 0,
            .imageSubresource = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
            .imageOffset = { 0, 0, 0 },
            .imageExtent = { f->w, f->h, 1 },
        };
        vkCmdCopyImageToBuffer(f->cmd, f->image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               f->readback, 1, &region);
        VkMemoryBarrier2 read_bar = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
            .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
        };
        khr_frame_barrier(f->cmd, nullptr, &read_bar);
    }
    if (vkEndCommandBuffer(f->cmd) != VK_SUCCESS) {
        goto done;
    }

    uint64_t point = f->next_point++;
    VkCommandBufferSubmitInfo cb_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = f->cmd,
    };
    VkSemaphoreSubmitInfo sig_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = f->timeline,
        .value = point,
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
    VkSubmitInfo2 submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &cb_info,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos = &sig_info,
    };
    /* Externally synchronized VkQueue: one alias across gfx/compute/xfer on
     * UMA silicon, so every submit serializes here. */
    khr_gfx_device_lock_queues(d);
    VkResult sub_res = vkQueueSubmit2(d->gfx_queue, 1, &submit, VK_NULL_HANDLE);
    khr_gfx_device_unlock_queues(d);
    if (sub_res != VK_SUCCESS) {
        goto done;
    }

    VkSemaphoreWaitInfo wait = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores = &f->timeline,
        .pValues = &point,
    };
    if (vkWaitSemaphores(d->device, &wait, KHR_FRAME_WAIT_NS) != VK_SUCCESS) {
        goto done;
    }

    if (f->use_host_copy) {
        /* Queue-free readback: the layout transition already happened on the
         * queue; the memcpy-class copy runs on the host, no transfer engine. */
        VkImageToMemoryCopy region = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_TO_MEMORY_COPY,
            .pHostPointer = f->readback_host,
            .memoryRowLength = 0,
            .memoryImageHeight = 0,
            .imageSubresource = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
            .imageOffset = { 0, 0, 0 },
            .imageExtent = { f->w, f->h, 1 },
        };
        VkCopyImageToMemoryInfo info = {
            .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_MEMORY_INFO,
            .flags = VK_HOST_IMAGE_COPY_MEMCPY_BIT,
            .srcImage = f->image,
            .srcImageLayout = VK_IMAGE_LAYOUT_GENERAL,
            .regionCount = 1,
            .pRegions = &region,
        };
        if (f->pfn_host_copy(d->device, &info) != VK_SUCCESS) {
            goto done;
        }
    }

    /* Center pixel of a fullscreen opaque card must read back red in BGRA. */
    uint8_t* px = (uint8_t*)f->readback_host +
                  ((size_t)(f->h / 2) * f->w + (f->w / 2)) * 4;
    out_bgra[0] = px[0];
    out_bgra[1] = px[1];
    out_bgra[2] = px[2];
    out_bgra[3] = px[3];
    ok = true;

done:
    khr_card_pipeline_destroy(&pipe);
    if (!ok) {
        out_bgra[0] = out_bgra[1] = out_bgra[2] = out_bgra[3] = 0;
    }
    return ok;
}

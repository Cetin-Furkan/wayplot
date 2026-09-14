#include "test_framework.h"
#include "khoros/gfx/device.h"
#include "khoros/gfx/depth.h"
#include "khoros/gfx/hiz.h"
#include "khoros/gfx/bda_arena.h"
#include "khoros/gfx/cull_pipeline.h"
#include <vulkan/vulkan.h>
#include <string.h>
#include <stdio.h>

[[nodiscard]]
bool test_hiz_occupancy_ratio(void) {
    khr_gfx_device_t dev = {};
    if (!khr_gfx_device_init(&dev, (dev_t)0)) {
        return false;
    }

    khr_bda_arena_t arena = {};
    constexpr size_t ARENA_SZ = 2 * 1024 * 1024;
    TEST_ASSERT(khr_bda_arena_init(&dev, &arena, ARENA_SZ), "Arena init failed");

    constexpr uint32_t NUM_INST = 256;
    khr_gpu_instance_t* insts = nullptr;
    VkDeviceAddress inst_addr = 0;
    TEST_ASSERT(khr_bda_arena_alloc(&arena, sizeof(khr_gpu_instance_t) * NUM_INST, 16,
                                    (void**)&insts, &inst_addr), "insts alloc failed");

    khr_gpu_culled_instance_t* culled = nullptr;
    VkDeviceAddress culled_addr = 0;
    TEST_ASSERT(khr_bda_arena_alloc(&arena, sizeof(khr_gpu_culled_instance_t) * NUM_INST, 16,
                                    (void**)&culled, &culled_addr), "culled alloc failed");

    khr_draw_indirect_cmd_t* draw_cmd = nullptr;
    VkDeviceAddress cmd_addr = 0;
    TEST_ASSERT(khr_bda_arena_alloc(&arena, sizeof(khr_draw_indirect_cmd_t), 16,
                                    (void**)&draw_cmd, &cmd_addr), "draw_cmd alloc failed");

    uint32_t* draw_count = nullptr;
    VkDeviceAddress count_addr = 0;
    TEST_ASSERT(khr_bda_arena_alloc(&arena, sizeof(uint32_t), 16,
                                    (void**)&draw_count, &count_addr), "draw_count alloc failed");

    uint32_t* vis_mask = nullptr;
    VkDeviceAddress vis_addr = 0;
    TEST_ASSERT(khr_bda_arena_alloc(&arena, sizeof(uint32_t) * NUM_INST, 16,
                                    (void**)&vis_mask, &vis_addr), "vis_mask alloc failed");

    /* 256 instances positioned behind an occluder slab (z in [-15, -30], within camera FOV) */
    for (uint32_t i = 0; i < NUM_INST; i++) {
        float row = (float)(i / 16) - 7.5f;
        float col = (float)(i % 16) - 7.5f;
        insts[i] = (khr_gpu_instance_t){
            .position = { col * 0.4f, row * 0.4f, -20.0f },
            .radius = 0.5f,
            .rotation = { 0.0f, 0.0f, 0.0f, 1.0f },
            .scale = { 1.0f, 1.0f, 1.0f },
            .mesh_id = 0,
            .albedo = { 0.8f, 0.8f, 0.8f },
            .roughness = 0.5f,
            .metallic = 0.0f,
            .ao = 1.0f,
            .albedo_tex_id = UINT32_MAX,
            .normal_tex_id = UINT32_MAX,
        };
    }

    constexpr uint32_t W = 256;
    constexpr uint32_t H = 256;
    khr_depth_target_t depth = {};
    TEST_ASSERT(khr_depth_target_create(&dev, &depth, W, H, VK_SAMPLE_COUNT_1_BIT,
                                        KHR_DEPTH_FORMAT_PRIMARY),
                "Depth create failed");
    khr_hiz_t hiz = {};
    TEST_ASSERT(khr_hiz_init(&hiz, &dev, W, H), "HiZ init failed");

    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = dev.cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    TEST_ASSERT_EQ(vkAllocateCommandBuffers(dev.device, &cbai, &cmd), VK_SUCCESS, "Alloc cmd failed");

    /* =========================================================================
     * Phase 1: Hi-Z OFF (Frustum culling only, flags = 0).
     * All 256 instances are in front of the camera and within FOV.
     * ========================================================================= */
    memset(draw_cmd, 0, sizeof(*draw_cmd));
    draw_cmd->vertexCount = 36;
    *draw_count = 0;
    memset(vis_mask, 0, sizeof(uint32_t) * NUM_INST);

    khr_hiz_cull_push_t push_off = {
        .instances_addr = inst_addr,
        .culled_instances_addr = culled_addr,
        .draw_cmd_addr = cmd_addr,
        .draw_count_addr = count_addr,
        .instance_count = NUM_INST,
        .index_count = 36,
        .alpha = 0.0f,
        .flags = 0U, /* Hi-Z disabled */
        .eye_fov = { 0.0f, 0.0f, 5.0f, 1.04719755f },
        .target_aspect = { 0.0f, 0.0f, 0.0f, 1.0f },
        .up_znear = { 0.0f, 1.0f, 0.0f, 0.1f },
        .visibility_addr = vis_addr,
        .hiz_width = W,
        .hiz_height = H,
        .hiz_mips = hiz.mip_levels,
    };

    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    TEST_ASSERT_EQ(vkBeginCommandBuffer(cmd, &bi), VK_SUCCESS, "Begin cmd failed");

    khr_hiz_cull_dispatch(&hiz, cmd, &push_off);

    TEST_ASSERT_EQ(vkEndCommandBuffer(cmd), VK_SUCCESS, "End cmd failed");

    khr_gfx_device_lock_queues(&dev);
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    TEST_ASSERT_EQ(vkQueueSubmit(dev.gfx_queue, 1, &si, VK_NULL_HANDLE), VK_SUCCESS, "Submit off failed");
    TEST_ASSERT_EQ(vkQueueWaitIdle(dev.gfx_queue), VK_SUCCESS, "WaitIdle off failed");
    khr_gfx_device_unlock_queues(&dev);

    uint32_t count_off = *draw_count;
    TEST_ASSERT_EQ(count_off, NUM_INST, "With Hi-Z off, all 256 instances must survive frustum culling");

    /* =========================================================================
     * Phase 2: Build Hi-Z Pyramid with an occluder slab in front (z = -2.0)
     * Reversed-Z depth of occluder is higher (closer to 1.0) than instances.
     * ========================================================================= */
    TEST_ASSERT_EQ(vkResetCommandBuffer(cmd, 0), VK_SUCCESS, "Reset cmd failed");
    TEST_ASSERT_EQ(vkBeginCommandBuffer(cmd, &bi), VK_SUCCESS, "Begin cmd 2 failed");

    /* Clear depth target with Reversed-Z occluder depth 0.90f (near plane) */
    VkImageMemoryBarrier pre_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .image = depth.image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &pre_barrier);

    VkClearDepthStencilValue clear_depth = { .depth = 0.90f, .stencil = 0 };
    VkImageSubresourceRange clear_range = {
        .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
    vkCmdClearDepthStencilImage(cmd, depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                &clear_depth, 1, &clear_range);

    VkImageMemoryBarrier post_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .image = depth.image,
        .subresourceRange = clear_range,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &post_barrier);

    /* Downsample depth buffer into Hi-Z pyramid */
    khr_hiz_build(&hiz, cmd, depth.image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

    /* Reset indirect draw command and count for Hi-Z ON evaluation */
    memset(draw_cmd, 0, sizeof(*draw_cmd));
    draw_cmd->vertexCount = 36;
    *draw_count = 0;
    memset(vis_mask, 0, sizeof(uint32_t) * NUM_INST);

    khr_hiz_cull_push_t push_on = push_off;
    push_on.flags = 4U; /* Bit 2: Hi-Z enabled */

    khr_hiz_cull_dispatch(&hiz, cmd, &push_on);

    TEST_ASSERT_EQ(vkEndCommandBuffer(cmd), VK_SUCCESS, "End cmd 2 failed");

    khr_gfx_device_lock_queues(&dev);
    TEST_ASSERT_EQ(vkQueueSubmit(dev.gfx_queue, 1, &si, VK_NULL_HANDLE), VK_SUCCESS, "Submit on failed");
    TEST_ASSERT_EQ(vkQueueWaitIdle(dev.gfx_queue), VK_SUCCESS, "WaitIdle on failed");
    khr_gfx_device_unlock_queues(&dev);

    uint32_t count_hiz = *draw_count;
    float alpha = (float)count_hiz / (float)count_off;

    printf("    [Hi-Z Occupancy] N=%u | count_off=%u | count_hiz=%u | ratio=%.3f (contract threshold <= 0.250)\n",
           NUM_INST, count_off, count_hiz, alpha);

    /* Fail unless count_hiz < alpha * count_off (alpha <= 0.25) */
    TEST_ASSERT(count_hiz <= (uint32_t)(0.25f * (float)count_off),
                "Hi-Z draw count must be <= 0.25 * count_off (all or most occluded instances culled)");

    vkFreeCommandBuffers(dev.device, dev.cmd_pool, 1, &cmd);
    khr_hiz_destroy(&hiz);
    khr_depth_target_destroy(&dev, &depth);
    khr_bda_arena_destroy(&dev, &arena);
    khr_gfx_device_destroy(&dev);
    return true;
}

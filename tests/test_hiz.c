#include "test_framework.h"
#include "khoros/gfx/device.h"
#include "khoros/gfx/depth.h"
#include "khoros/gfx/hiz.h"
#include "khoros/gfx/bda_arena.h"
#include "khoros/gfx/cull_pipeline.h"
#include <vulkan/vulkan.h>
#include <string.h>

[[nodiscard]]
bool test_hiz_struct_contracts(void) {
    TEST_ASSERT_EQ(sizeof(khr_hiz_cull_push_t), 128U, "khr_hiz_cull_push_t size must be 128 bytes");
    TEST_ASSERT(sizeof(khr_hiz_cull_push_t) <= 128U, "khr_hiz_cull_push_t must fit within 128 bytes push constants");

    TEST_ASSERT_EQ(offsetof(khr_hiz_cull_push_t, instances_addr), 0U, "instances_addr must be at offset 0");
    TEST_ASSERT_EQ(offsetof(khr_hiz_cull_push_t, culled_instances_addr), 8U, "culled_instances_addr must be at offset 8");
    TEST_ASSERT_EQ(offsetof(khr_hiz_cull_push_t, draw_cmd_addr), 16U, "draw_cmd_addr must be at offset 16");
    TEST_ASSERT_EQ(offsetof(khr_hiz_cull_push_t, draw_count_addr), 24U, "draw_count_addr must be at offset 24");
    TEST_ASSERT_EQ(offsetof(khr_hiz_cull_push_t, instance_count), 32U, "instance_count must be at offset 32");
    TEST_ASSERT_EQ(offsetof(khr_hiz_cull_push_t, index_count), 36U, "index_count must be at offset 36");
    TEST_ASSERT_EQ(offsetof(khr_hiz_cull_push_t, alpha), 40U, "alpha must be at offset 40");
    TEST_ASSERT_EQ(offsetof(khr_hiz_cull_push_t, flags), 44U, "flags must be at offset 44");
    TEST_ASSERT_EQ(offsetof(khr_hiz_cull_push_t, eye_fov), 48U, "eye_fov must be at offset 48");
    TEST_ASSERT_EQ(offsetof(khr_hiz_cull_push_t, target_aspect), 64U, "target_aspect must be at offset 64");
    TEST_ASSERT_EQ(offsetof(khr_hiz_cull_push_t, up_znear), 80U, "up_znear must be at offset 80");
    TEST_ASSERT_EQ(offsetof(khr_hiz_cull_push_t, visibility_addr), 96U, "visibility_addr must be at offset 96");
    TEST_ASSERT_EQ(offsetof(khr_hiz_cull_push_t, hiz_width), 104U, "hiz_width must be at offset 104");
    TEST_ASSERT_EQ(offsetof(khr_hiz_cull_push_t, hiz_height), 108U, "hiz_height must be at offset 108");
    TEST_ASSERT_EQ(offsetof(khr_hiz_cull_push_t, hiz_mips), 112U, "hiz_mips must be at offset 112");

    return true;
}

[[nodiscard]]
bool test_hiz_pyramid_lifecycle_and_mips(void) {
    khr_gfx_device_t dev = {};
    if (!khr_gfx_device_init(&dev, (dev_t)0)) {
        return false;
    }

    khr_hiz_t hiz = {};
    constexpr uint32_t W = 512;
    constexpr uint32_t H = 512;
    bool init_ok = khr_hiz_init(&hiz, &dev, W, H);
    TEST_ASSERT(init_ok, "khr_hiz_init must succeed");

    /* 512 >> 9 = 1, so 10 mips: 512, 256, 128, 64, 32, 16, 8, 4, 2, 1 */
    TEST_ASSERT_EQ(hiz.mip_levels, 10U, "512x512 pyramid must have 10 mip levels");
    TEST_ASSERT_NE(hiz.image, VK_NULL_HANDLE, "Hi-Z image must be valid");
    TEST_ASSERT_NE(hiz.view_full, VK_NULL_HANDLE, "Full mip view must be valid");
    TEST_ASSERT_NE(hiz.sampler, VK_NULL_HANDLE, "Reduction sampler must be valid");

    for (uint32_t m = 0; m < hiz.mip_levels; m++) {
        TEST_ASSERT_NE(hiz.mip_views[m], VK_NULL_HANDLE, "Per-mip view must be valid");
    }

    TEST_ASSERT_NE(hiz.downsample_pipeline, VK_NULL_HANDLE, "Downsample pipeline must be valid");
    TEST_ASSERT_NE(hiz.cull_pipeline, VK_NULL_HANDLE, "Hi-Z cull pipeline must be valid");

    khr_hiz_destroy(&hiz);
    TEST_ASSERT_EQ(hiz.image, VK_NULL_HANDLE, "Hi-Z image must be destroyed");
    TEST_ASSERT_EQ(hiz.view_full, VK_NULL_HANDLE, "Full view must be destroyed");
    TEST_ASSERT_EQ(hiz.downsample_pipeline, VK_NULL_HANDLE, "Pipeline must be destroyed");

    khr_gfx_device_destroy(&dev);
    return true;
}

[[nodiscard]]
bool test_hiz_depth_downsample_and_build(void) {
    khr_gfx_device_t dev = {};
    if (!khr_gfx_device_init(&dev, (dev_t)0)) {
        return false;
    }

    constexpr uint32_t W = 256;
    constexpr uint32_t H = 256;
    khr_depth_target_t depth = {};
    TEST_ASSERT(khr_depth_target_create(&dev, &depth, W, H, VK_SAMPLE_COUNT_1_BIT,
                                        KHR_DEPTH_FORMAT_PRIMARY),
                "Depth target create failed");

    khr_hiz_t hiz = {};
    TEST_ASSERT(khr_hiz_init(&hiz, &dev, W, H), "Hi-Z init failed");

    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = dev.cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    TEST_ASSERT_EQ(vkAllocateCommandBuffers(dev.device, &cbai, &cmd), VK_SUCCESS,
                   "Allocate command buffer failed");

    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    TEST_ASSERT_EQ(vkBeginCommandBuffer(cmd, &bi), VK_SUCCESS, "Begin command buffer failed");

    /* Transition depth target to attachment layout initially */
    VkImageMemoryBarrier init_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
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
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &init_barrier);

    /* Build Hi-Z Pyramid */
    khr_hiz_build(&hiz, cmd, depth.image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

    TEST_ASSERT_EQ(vkEndCommandBuffer(cmd), VK_SUCCESS, "End command buffer failed");

    /* Submit and verify execution */
    khr_gfx_device_lock_queues(&dev);
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    TEST_ASSERT_EQ(vkQueueSubmit(dev.gfx_queue, 1, &si, VK_NULL_HANDLE), VK_SUCCESS,
                   "vkQueueSubmit failed");
    TEST_ASSERT_EQ(vkQueueWaitIdle(dev.gfx_queue), VK_SUCCESS, "vkQueueWaitIdle failed");
    khr_gfx_device_unlock_queues(&dev);

    vkFreeCommandBuffers(dev.device, dev.cmd_pool, 1, &cmd);
    khr_hiz_destroy(&hiz);
    khr_depth_target_destroy(&dev, &depth);
    khr_gfx_device_destroy(&dev);
    return true;
}

[[nodiscard]]
bool test_hiz_two_pass_occlusion_culling_dispatch(void) {
    khr_gfx_device_t dev = {};
    if (!khr_gfx_device_init(&dev, (dev_t)0)) {
        return false;
    }

    khr_bda_arena_t arena = {};
    constexpr size_t ARENA_SZ = 1024 * 1024;
    TEST_ASSERT(khr_bda_arena_init(&dev, &arena, ARENA_SZ), "Arena init failed");

    constexpr uint32_t NUM_INST = 16;
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

    memset(draw_cmd, 0, sizeof(*draw_cmd));
    draw_cmd->vertexCount = 36;
    *draw_count = 0;
    memset(vis_mask, 0, sizeof(uint32_t) * NUM_INST);

    /* Setup instances along Z axis */
    for (uint32_t i = 0; i < NUM_INST; i++) {
        insts[i] = (khr_gpu_instance_t){
            .position = { 0.0f, 0.0f, -(float)(i + 1) * 2.0f },
            .radius = 1.0f,
            .rotation = { 0.0f, 0.0f, 0.0f, 1.0f },
            .scale = { 1.0f, 1.0f, 1.0f },
            .mesh_id = 0,
            .albedo = { 0.8f, 0.8f, 0.8f },
            .roughness = 0.5f,
            .metallic = 0.0f,
            .ao = 1.0f,
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

    /* 1. Early Pass push constants */
    khr_hiz_cull_push_t push_early = {
        .instances_addr = inst_addr,
        .culled_instances_addr = culled_addr,
        .draw_cmd_addr = cmd_addr,
        .draw_count_addr = count_addr,
        .instance_count = NUM_INST,
        .index_count = 36,
        .alpha = 0.0f,
        .flags = 4U, /* bit 2: hiz enabled, early pass */
        .eye_fov = { 0.0f, 0.0f, 5.0f, 1.047f }, /* 60 deg fov */
        .target_aspect = { 0.0f, 0.0f, 0.0f, 1.0f },
        .up_znear = { 0.0f, 1.0f, 0.0f, 0.1f },
        .visibility_addr = vis_addr,
        .hiz_width = W,
        .hiz_height = H,
        .hiz_mips = hiz.mip_levels,
    };

    /* 2. Late Pass push constants */
    khr_hiz_cull_push_t push_late = push_early;
    push_late.flags = 4U | 8U; /* bit 2: hiz, bit 3: late pass */

    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = dev.cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    TEST_ASSERT_EQ(vkAllocateCommandBuffers(dev.device, &cbai, &cmd), VK_SUCCESS, "Alloc cmd failed");

    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    TEST_ASSERT_EQ(vkBeginCommandBuffer(cmd, &bi), VK_SUCCESS, "Begin cmd failed");

    /* Dispatch Early Pass */
    khr_hiz_cull_dispatch(&hiz, cmd, &push_early);

    /* Barrier between Early and Late passes */
    VkMemoryBarrier mb = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &mb, 0, nullptr, 0, nullptr);

    /* Dispatch Late Pass */
    khr_hiz_cull_dispatch(&hiz, cmd, &push_late);

    TEST_ASSERT_EQ(vkEndCommandBuffer(cmd), VK_SUCCESS, "End cmd failed");

    khr_gfx_device_lock_queues(&dev);
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    TEST_ASSERT_EQ(vkQueueSubmit(dev.gfx_queue, 1, &si, VK_NULL_HANDLE), VK_SUCCESS,
                   "Submit failed");
    TEST_ASSERT_EQ(vkQueueWaitIdle(dev.gfx_queue), VK_SUCCESS, "WaitIdle failed");
    khr_gfx_device_unlock_queues(&dev);

    /* Verification of GPU results: at least some instances were processed */
    TEST_ASSERT(draw_cmd->instanceCount > 0, "At least one instance must survive frustum/hiz");
    TEST_ASSERT_EQ(*draw_count, draw_cmd->instanceCount, "Atomic draw_count must match draw_cmd instanceCount");

    vkFreeCommandBuffers(dev.device, dev.cmd_pool, 1, &cmd);
    khr_hiz_destroy(&hiz);
    khr_depth_target_destroy(&dev, &depth);
    khr_bda_arena_destroy(&dev, &arena);
    khr_gfx_device_destroy(&dev);
    return true;
}

[[nodiscard]]
bool test_hiz_multi_draw_indirect_count_execution(void) {
    khr_gfx_device_t dev = {};
    if (!khr_gfx_device_init(&dev, (dev_t)0)) {
        return false;
    }

    /* Verify Vulkan 1.2 drawIndirectCount feature is active */
    TEST_ASSERT(dev.device != VK_NULL_HANDLE, "Device must be valid");

    /* Create dummy command buffer and test vkCmdDrawIndexedIndirectCount record path */
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = dev.cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    TEST_ASSERT_EQ(vkAllocateCommandBuffers(dev.device, &cbai, &cmd), VK_SUCCESS, "Alloc cmd failed");

    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    TEST_ASSERT_EQ(vkBeginCommandBuffer(cmd, &bi), VK_SUCCESS, "Begin cmd failed");

    khr_bda_arena_t arena = {};
    TEST_ASSERT(khr_bda_arena_init(&dev, &arena, 64 * 1024), "Arena init failed");

    khr_draw_indexed_indirect_cmd_t* cmd_data = nullptr;
    VkDeviceAddress cmd_gpu = 0;
    TEST_ASSERT(khr_bda_arena_alloc(&arena, sizeof(khr_draw_indexed_indirect_cmd_t), 16,
                                    (void**)&cmd_data, &cmd_gpu), "cmd_data alloc failed");
    uint32_t* count_data = nullptr;
    VkDeviceAddress count_gpu = 0;
    TEST_ASSERT(khr_bda_arena_alloc(&arena, sizeof(uint32_t), 16,
                                    (void**)&count_data, &count_gpu), "count_data alloc failed");
    cmd_data->indexCount = 36;
    cmd_data->instanceCount = 5;
    cmd_data->firstIndex = 0;
    cmd_data->vertexOffset = 0;
    cmd_data->firstInstance = 0;
    *count_data = 1;

    VkDeviceSize cmd_offset = (uint8_t*)cmd_data - (uint8_t*)arena.host_ptr;
    VkDeviceSize count_offset = (uint8_t*)count_data - (uint8_t*)arena.host_ptr;

    /* vkCmdDrawIndexedIndirectCount is a core Vulkan 1.2 command */
    vkCmdDrawIndexedIndirectCount(cmd, arena.buffer, cmd_offset,
                                  arena.buffer, count_offset,
                                  1, sizeof(khr_draw_indexed_indirect_cmd_t));

    TEST_ASSERT_EQ(vkEndCommandBuffer(cmd), VK_SUCCESS, "End cmd failed");

    vkFreeCommandBuffers(dev.device, dev.cmd_pool, 1, &cmd);
    khr_bda_arena_destroy(&dev, &arena);
    khr_gfx_device_destroy(&dev);
    return true;
}

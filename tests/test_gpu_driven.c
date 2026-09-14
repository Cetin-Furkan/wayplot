#include "test_framework.h"
#include "khoros/gfx/device.h"
#include "khoros/gfx/bda_arena.h"
#include "khoros/gfx/gpu_math.h"
#include "khoros/gfx/cull_pipeline.h"
#include "khoros/gfx/depth.h"
#include <math.h>
#include <string.h>

[[nodiscard]]
bool test_gpu_math_struct_contracts(void) {
    TEST_ASSERT_EQ(sizeof(khr_gpu_mat4_t), 64U, "khr_gpu_mat4_t size must be 64 bytes");
    TEST_ASSERT_EQ(sizeof(khr_gpu_instance_t), 80U, "khr_gpu_instance_t size must be 80 bytes");
    TEST_ASSERT_EQ(sizeof(khr_gpu_culled_instance_t), 192U, "khr_gpu_culled_instance_t size must be 192 bytes");
    TEST_ASSERT_EQ(sizeof(khr_draw_indirect_cmd_t), 16U, "khr_draw_indirect_cmd_t size must be 16 bytes");
    TEST_ASSERT_EQ(sizeof(khr_cull_push_t), 96U, "khr_cull_push_t size must be 96 bytes");
    TEST_ASSERT_EQ(sizeof(khr_mesh_instanced_push_t), 112U, "khr_mesh_instanced_push_t size must be 112 bytes");

    TEST_ASSERT(sizeof(khr_cull_push_t) <= 128U, "khr_cull_push_t must fit within 128 bytes push constants");
    TEST_ASSERT(sizeof(khr_mesh_instanced_push_t) <= 128U, "khr_mesh_instanced_push_t must fit within 128 bytes push constants");

    TEST_ASSERT_EQ(alignof(khr_gpu_mat4_t), 16U, "khr_gpu_mat4_t must have 16-byte alignment");
    TEST_ASSERT_EQ(alignof(khr_gpu_instance_t), 16U, "khr_gpu_instance_t must have 16-byte alignment");
    TEST_ASSERT_EQ(alignof(khr_gpu_culled_instance_t), 16U, "khr_gpu_culled_instance_t must have 16-byte alignment");

    TEST_ASSERT_EQ(offsetof(khr_cull_push_t, instances_addr) % 8, 0U, "instances_addr 8-byte alignment");
    TEST_ASSERT_EQ(offsetof(khr_cull_push_t, culled_instances_addr) % 8, 0U, "culled_instances_addr 8-byte alignment");
    TEST_ASSERT_EQ(offsetof(khr_cull_push_t, draw_cmd_addr) % 8, 0U, "draw_cmd_addr 8-byte alignment");

    /* Non-Uniform Scale Normal Transformation Contract:
     * Surface tangent T = (1, 1, 0) in local space.
     * Perpendicular local normal N = (-1, 1, 0) / sqrt(2). (dot(T, N) = 0).
     * Scale non-uniformly by S = (2.0, 0.5, 1.0).
     * Tangent transforms by S: T' = S * T = (2.0, 0.5, 0).
     * The true world normal must remain orthogonal to T': dot(T', N') == 0.
     * Skewed model multiplication yields: S * N = (-2.0, 0.5, 0) / sqrt(2) -> dot(T', S*N) = -3.75 != 0.
     * Inverse scale transformation yields: N' = N / S = (-0.5, 2.0, 0) / sqrt(2) -> dot(T', N') = 0.
     */
    float tangent_world[3] = { 2.0f * 1.0f, 0.5f * 1.0f, 0.0f };
    float inv_scaled_normal[3] = { (-1.0f / sqrtf(2.0f)) / 2.0f, (1.0f / sqrtf(2.0f)) / 0.5f, 0.0f };
    float dot_prod = tangent_world[0] * inv_scaled_normal[0] +
                     tangent_world[1] * inv_scaled_normal[1] +
                     tangent_world[2] * inv_scaled_normal[2];
    TEST_ASSERT(fabsf(dot_prod) < 1e-6f, "Inverse-scale normal must remain strictly orthogonal to transformed tangent");
    return true;
}

[[nodiscard]]
bool test_gpu_cull_pipeline_lifecycle(void) {
    khr_gfx_device_t dev = {};
    TEST_ASSERT(khr_gfx_device_init(&dev, (dev_t)0), "gfx device init");

    khr_cull_pipeline_t cull_pipe = {};
    TEST_ASSERT(khr_cull_pipeline_init(&cull_pipe, &dev), "cull pipeline init");
    TEST_ASSERT_NOT_NULL(cull_pipe.pipeline, "cull pipeline not null");
    TEST_ASSERT_NOT_NULL(cull_pipe.layout, "cull layout not null");
    TEST_ASSERT_NOT_NULL(cull_pipe.comp_module, "cull module not null");

    khr_cull_pipeline_destroy(&cull_pipe);
    TEST_ASSERT_NULL(cull_pipe.pipeline, "cull pipeline null after destroy");

    khr_gfx_device_destroy(&dev);
    return true;
}

[[nodiscard]]
bool test_gpu_mesh_instanced_pipeline_lifecycle(void) {
    khr_gfx_device_t dev = {};
    TEST_ASSERT(khr_gfx_device_init(&dev, (dev_t)0), "gfx device init");

    khr_mesh_instanced_pipeline_t inst_pipe = {};
    TEST_ASSERT(khr_mesh_instanced_pipeline_init(&inst_pipe, &dev, VK_FORMAT_B8G8R8A8_UNORM, nullptr),
                "instanced mesh pipeline init");
    TEST_ASSERT_NOT_NULL(inst_pipe.pipeline, "pipeline not null");
    TEST_ASSERT_NOT_NULL(inst_pipe.layout, "layout not null");

    khr_mesh_instanced_pipeline_destroy(&inst_pipe);
    TEST_ASSERT_NULL(inst_pipe.pipeline, "pipeline null after destroy");

    khr_gfx_device_destroy(&dev);
    return true;
}

[[nodiscard]]
bool test_gpu_compute_math_and_frustum_culling(void) {
    khr_gfx_device_t dev = {};
    TEST_ASSERT(khr_gfx_device_init(&dev, (dev_t)0), "gfx device init");

    khr_bda_arena_t arena = {};
    TEST_ASSERT(khr_bda_arena_init(&dev, &arena, 64 * 1024), "arena init");

    /* Allocate BDA buffers */
    void* inst_host = nullptr;
    VkDeviceAddress inst_gpu = 0;
    TEST_ASSERT(khr_bda_arena_alloc(&arena, 4 * sizeof(khr_gpu_instance_t), 64, &inst_host, &inst_gpu), "alloc instances");

    void* culled_host = nullptr;
    VkDeviceAddress culled_gpu = 0;
    TEST_ASSERT(khr_bda_arena_alloc(&arena, 4 * sizeof(khr_gpu_culled_instance_t), 64, &culled_host, &culled_gpu), "alloc culled");

    void* cmd_host = nullptr;
    VkDeviceAddress cmd_gpu = 0;
    TEST_ASSERT(khr_bda_arena_alloc(&arena, sizeof(khr_draw_indirect_cmd_t), 64, &cmd_host, &cmd_gpu), "alloc cmd");

    void* count_host = nullptr;
    VkDeviceAddress count_gpu = 0;
    TEST_ASSERT(khr_bda_arena_alloc(&arena, sizeof(uint32_t), 64, &count_host, &count_gpu), "alloc count");

    /* Populate instances:
     * Instance 0: at (0, 0, 0), radius 0.5 (in front of camera, inside frustum) -> VISIBLE
     * Instance 1: at (0, 0, 20), radius 0.5 (behind camera) -> CULLED
     * Instance 2: at (100, 0, 0), radius 0.5 (far right) -> CULLED
     * Instance 3: at (0.2, 0.2, 1.0), radius 0.5 (inside frustum) -> VISIBLE
     */
    khr_gpu_instance_t* instances = (khr_gpu_instance_t*)inst_host;
    instances[0] = (khr_gpu_instance_t){
        .position = { 0.0f, 0.0f, 0.0f },
        .radius = 0.5f,
        .rotation = { 0.0f, 0.0f, 0.0f, 1.0f },
        .scale = { 1.0f, 1.0f, 1.0f },
        .mesh_id = 0,
        .albedo = { 0.8f, 0.4f, 0.2f },
        .roughness = 0.35f,
        .metallic = 0.95f,
        .ao = 1.0f,
    };
    instances[1] = (khr_gpu_instance_t){
        .position = { 0.0f, 0.0f, 20.0f },
        .radius = 0.5f,
        .rotation = { 0.0f, 0.0f, 0.0f, 1.0f },
        .scale = { 1.0f, 1.0f, 1.0f },
        .mesh_id = 0,
    };
    instances[2] = (khr_gpu_instance_t){
        .position = { 100.0f, 0.0f, 0.0f },
        .radius = 0.5f,
        .rotation = { 0.0f, 0.0f, 0.0f, 1.0f },
        .scale = { 1.0f, 1.0f, 1.0f },
        .mesh_id = 0,
    };
    instances[3] = (khr_gpu_instance_t){
        .position = { 0.2f, 0.2f, 1.0f },
        .radius = 0.5f,
        .rotation = { 0.0f, 0.70710678f, 0.0f, 0.70710678f }, /* 90-degree Y rotation */
        .scale = { 1.0f, 1.0f, 1.0f },
        .mesh_id = 0,
    };

    /* Initialize draw command */
    khr_draw_indirect_cmd_t* draw_cmd = (khr_draw_indirect_cmd_t*)cmd_host;
    *draw_cmd = (khr_draw_indirect_cmd_t){
        .vertexCount = 36,
        .instanceCount = 0, /* GPU will atomically increment this! */
        .firstVertex = 0,
        .firstInstance = 0,
    };

    uint32_t* draw_count = (uint32_t*)count_host;
    *draw_count = 0;

    memset(culled_host, 0, 4 * sizeof(khr_gpu_culled_instance_t));

    /* Initialize cull pipeline */
    khr_cull_pipeline_t cull_pipe = {};
    TEST_ASSERT(khr_cull_pipeline_init(&cull_pipe, &dev), "cull pipe init");

    /* Allocate command buffer */
    VkCommandBufferAllocateInfo cb_ai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = dev.pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    TEST_ASSERT_EQ(vkAllocateCommandBuffers(dev.device, &cb_ai, &cmd), VK_SUCCESS, "alloc cmd buffer");

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    TEST_ASSERT_EQ(vkBeginCommandBuffer(cmd, &begin_info), VK_SUCCESS, "begin cmd");

    khr_cull_push_t push = {
        .instances_addr = inst_gpu,
        .culled_instances_addr = culled_gpu,
        .draw_cmd_addr = cmd_gpu,
        .draw_count_addr = count_gpu,
        .instance_count = 4,
        .index_count = 36,
        .pad0 = 0,
        .pad1 = 0,
        .eye_fov = { 0.0f, 0.0f, 5.0f, 1.04719755f },
        .target_aspect = { 0.0f, 0.0f, 0.0f, 1.0f },
        .up_znear = { 0.0f, 1.0f, 0.0f, 0.1f },
    };

    khr_cull_pipeline_dispatch(&cull_pipe, cmd, &push);

    /* Memory barrier: Compute write -> Host read */
    VkMemoryBarrier2 mb = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
        .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
    };
    VkDependencyInfo dep = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &mb,
    };
    vkCmdPipelineBarrier2(cmd, &dep);

    TEST_ASSERT_EQ(vkEndCommandBuffer(cmd), VK_SUCCESS, "end cmd");

    /* Submit with timeline semaphore */
    uint64_t wait_val = dev.acquire_point;
    dev.acquire_point++;
    uint64_t sig_val = dev.acquire_point;

    VkTimelineSemaphoreSubmitInfo timeline_info = {
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .waitSemaphoreValueCount = (wait_val > 0) ? 1 : 0,
        .pWaitSemaphoreValues = (wait_val > 0) ? &wait_val : nullptr,
        .signalSemaphoreValueCount = 1,
        .pSignalSemaphoreValues = &sig_val,
    };
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = &timeline_info,
        .waitSemaphoreCount = (wait_val > 0) ? 1 : 0,
        .pWaitSemaphores = (wait_val > 0) ? &dev.acquire_sem : nullptr,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &dev.acquire_sem,
    };

    TEST_ASSERT_EQ(vkQueueSubmit(dev.gfx_queue, 1, &si, VK_NULL_HANDLE), VK_SUCCESS, "queue submit");

    /* Wait on timeline semaphore */
    VkSemaphoreWaitInfo wi = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores = &dev.acquire_sem,
        .pValues = &sig_val,
    };
    TEST_ASSERT_EQ(vkWaitSemaphores(dev.device, &wi, 5'000'000'000ULL), VK_SUCCESS, "wait semaphore");

    /* Readback assertions: Exactly 2 instances must be visible! */
    TEST_ASSERT_EQ(draw_cmd->instanceCount, 2U, "GPU must cull exactly 2 instances and keep 2 visible");
    TEST_ASSERT_EQ(*draw_count, 2U, "GPU atomic counter must equal 2");
    TEST_ASSERT_EQ(draw_cmd->vertexCount, 36U, "vertexCount must remain 36");

    /* Verify GPU-computed Reversed-Z MVP Matrix for Instance 0 (at 0, 0, 0) */
    khr_gpu_culled_instance_t* culled = (khr_gpu_culled_instance_t*)culled_host;

    /* Transform origin (0, 0, 0, 1) by GPU culled[0].mvp:
     * v_clip = c0*0 + c1*0 + c2*0 + c3*1 = c3
     */
    float clip_z = culled[0].mvp.c3[2];
    float clip_w = culled[0].mvp.c3[3];

    TEST_ASSERT(clip_w > 0.0f, "w_clip must be positive (in front of camera)");
    float z_ndc = clip_z / clip_w;

    /* For eye at (0, 0, 5) and target at (0, 0, 0), distance d = 5.0.
     * Analytical Reversed-Z depth with z_near = 0.1:
     * Z_ndc = z_near / d = 0.1 / 5.0 = 0.02.
     */
    TEST_ASSERT(z_ndc > 0.0f && z_ndc <= 1.0f, "Reversed-Z depth must be within (0, 1]");
    TEST_ASSERT(fabsf(z_ndc - 0.02f) < 0.001f, "GPU calculated analytical Reversed-Z depth precisely");

    /* Verify GPU-propagated PBR Material Attributes */
    TEST_ASSERT(fabsf(culled[0].albedo[0] - 0.8f) < 0.001f, "GPU preserved albedo.r");
    TEST_ASSERT(fabsf(culled[0].albedo[1] - 0.4f) < 0.001f, "GPU preserved albedo.g");
    TEST_ASSERT(fabsf(culled[0].albedo[2] - 0.2f) < 0.001f, "GPU preserved albedo.b");
    TEST_ASSERT(fabsf(culled[0].roughness - 0.35f) < 0.001f, "GPU preserved roughness");
    TEST_ASSERT(fabsf(culled[0].metallic - 0.95f) < 0.001f, "GPU preserved metallic");
    TEST_ASSERT(fabsf(culled[0].ao - 1.0f) < 0.001f, "GPU preserved ao");
    TEST_ASSERT(fabsf(culled[0].rotation[3] - 1.0f) < 0.001f, "GPU preserved rotation.w");
    TEST_ASSERT(fabsf(culled[0].scale[0] - 1.0f) < 0.001f, "GPU preserved scale.x");

    /* Cleanup */
    vkFreeCommandBuffers(dev.device, dev.pool, 1, &cmd);
    khr_cull_pipeline_destroy(&cull_pipe);
    khr_bda_arena_destroy(&dev, &arena);
    khr_gfx_device_destroy(&dev);
    return true;
}

[[nodiscard]]
bool test_gpu_silicon_timestamp_queries(void) {
    khr_gfx_device_t dev = {};
    TEST_ASSERT(khr_gfx_device_init(&dev, (dev_t)0), "gfx device init");
    TEST_ASSERT_GT(dev.timestamp_period, 0.0f, "timestampPeriod must be > 0.0 ns");
    TEST_ASSERT(dev.has_timestamps, "GPU must support graphics & compute timestamps");

    VkQueryPoolCreateInfo qpci = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount = 2,
    };
    VkQueryPool qpool = VK_NULL_HANDLE;
    TEST_ASSERT_EQ(vkCreateQueryPool(dev.device, &qpci, nullptr, &qpool), VK_SUCCESS, "create query pool");
    TEST_ASSERT_NOT_NULL(qpool, "query pool not null");

    VkCommandBufferAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = dev.pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    TEST_ASSERT_EQ(vkAllocateCommandBuffers(dev.device, &ai, &cmd), VK_SUCCESS, "alloc cmd");

    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    TEST_ASSERT_EQ(vkBeginCommandBuffer(cmd, &bi), VK_SUCCESS, "begin cmd");

    vkCmdResetQueryPool(cmd, qpool, 0, 2);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qpool, 0);

    /* Insert an execution barrier to generate non-zero GPU execution work */
    VkMemoryBarrier2 bar = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .srcAccessMask = 0,
        .dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
        .dstAccessMask = 0,
    };
    VkDependencyInfo dep = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &bar,
    };
    vkCmdPipelineBarrier2(cmd, &dep);

    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qpool, 1);
    TEST_ASSERT_EQ(vkEndCommandBuffer(cmd), VK_SUCCESS, "end cmd");

    uint64_t sig_val = dev.acquire_point + 1U;
    dev.acquire_point = sig_val;

    VkCommandBufferSubmitInfo csi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = cmd,
    };
    VkSemaphoreSubmitInfo ssi = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = dev.acquire_sem,
        .value = sig_val,
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
    VkSubmitInfo2 si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &csi,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos = &ssi,
    };
    TEST_ASSERT_EQ(vkQueueSubmit2(dev.gfx_queue, 1, &si, VK_NULL_HANDLE), VK_SUCCESS, "queue submit2");

    VkSemaphoreWaitInfo wi = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores = &dev.acquire_sem,
        .pValues = &sig_val,
    };
    TEST_ASSERT_EQ(vkWaitSemaphores(dev.device, &wi, 5'000'000'000ULL), VK_SUCCESS, "wait semaphore");

    uint64_t ts[2] = {};
    TEST_ASSERT_EQ(vkGetQueryPoolResults(dev.device, qpool, 0, 2, sizeof(ts), ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT),
                   VK_SUCCESS, "get query pool results");
    TEST_ASSERT_GE(ts[1], ts[0], "bottom timestamp must be >= top timestamp");

    uint64_t delta_ns = (uint64_t)((double)(ts[1] - ts[0]) * (double)dev.timestamp_period);
    TEST_ASSERT_GE(delta_ns, 0ULL, "delta_ns >= 0");

    vkFreeCommandBuffers(dev.device, dev.pool, 1, &cmd);
    vkDestroyQueryPool(dev.device, qpool, nullptr);
    khr_gfx_device_destroy(&dev);
    return true;
}

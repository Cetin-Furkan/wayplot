#include "test_framework.h"
#include "khoros/gfx/device.h"
#include "khoros/gfx/bda_arena.h"
#include "khoros/gfx/depth.h"
#include "khoros/gfx/gpu_math.h"
#include "khoros/gfx/camera.h"
#include "khoros/gfx/cull_pipeline.h"
#include "khoros/gfx/scene.h"
#include <math.h>
#include <string.h>

[[nodiscard]]
bool test_light_struct_contracts(void) {
    TEST_ASSERT_EQ(sizeof(khr_gpu_light_t), 64U, "khr_gpu_light_t must be 64 bytes");
    TEST_ASSERT_EQ(alignof(khr_gpu_light_t), 16U, "khr_gpu_light_t must have 16-byte alignment");

    TEST_ASSERT_EQ(sizeof(khr_mesh_instanced_push_t), 112U, "khr_mesh_instanced_push_t must be 112 bytes");
    TEST_ASSERT(sizeof(khr_mesh_instanced_push_t) <= 128U, "khr_mesh_instanced_push_t within 128 bytes limit");

    TEST_ASSERT_EQ(offsetof(khr_gpu_light_t, position), 0U, "position at offset 0");
    TEST_ASSERT_EQ(offsetof(khr_gpu_light_t, type), 12U, "type at offset 12");
    TEST_ASSERT_EQ(offsetof(khr_gpu_light_t, direction), 16U, "direction at offset 16");
    TEST_ASSERT_EQ(offsetof(khr_gpu_light_t, range), 28U, "range at offset 28");
    TEST_ASSERT_EQ(offsetof(khr_gpu_light_t, color), 32U, "color at offset 32");
    TEST_ASSERT_EQ(offsetof(khr_gpu_light_t, intensity), 44U, "intensity at offset 44");
    TEST_ASSERT_EQ(offsetof(khr_gpu_light_t, spot_inner), 48U, "spot_inner at offset 48");
    TEST_ASSERT_EQ(offsetof(khr_gpu_light_t, spot_outer), 52U, "spot_outer at offset 52");

    TEST_ASSERT_EQ(offsetof(khr_mesh_instanced_push_t, lights_addr), 24U, "lights_addr at offset 24");
    TEST_ASSERT_EQ(offsetof(khr_mesh_instanced_push_t, light_count), 88U, "light_count at offset 88");
    TEST_ASSERT_EQ(offsetof(khr_mesh_instanced_push_t, normals_addr), 96U, "normals_addr at offset 96");

    return true;
}

/* Analytical verification of physically-based inverse-square distance attenuation */
static float calculate_phys_attenuation(float d, float range) {
    if (d >= range || d < 1.0e-5f) {
        return 0.0f;
    }
    float ratio = d / range;
    float ratio4 = ratio * ratio * ratio * ratio;
    float f = 1.0f - ratio4;
    if (f < 0.0f) f = 0.0f;
    float window = f * f;
    return window / (d * d);
}

[[nodiscard]]
bool test_physically_based_light_falloff(void) {
    constexpr float R = 20.0f;

    float att_1m = calculate_phys_attenuation(1.0f, R);
    float att_2m = calculate_phys_attenuation(2.0f, R);
    float att_4m = calculate_phys_attenuation(4.0f, R);

    TEST_ASSERT(att_1m > 0.0f, "att_1m must be positive");
    TEST_ASSERT(att_2m > 0.0f, "att_2m must be positive");

    /* Inverse-Square Law: Doubling distance from 1m to 2m quarters irradiance (ratio = 0.25) */
    float ratio_2_to_1 = att_2m / att_1m;
    TEST_ASSERT(fabsf(ratio_2_to_1 - 0.25f) < 0.005f, "Doubling distance quarters attenuation (1/d^2 law)");

    /* Quadrupling distance from 1m to 4m yields 1/16 irradiance (ratio = 0.0625) */
    float ratio_4_to_1 = att_4m / att_1m;
    TEST_ASSERT(fabsf(ratio_4_to_1 - 0.0625f) < 0.005f, "Quadrupling distance yields 1/16 attenuation (1/d^2 law)");

    /* Smooth cutoff at radius R */
    float att_at_r = calculate_phys_attenuation(R, R);
    TEST_ASSERT_EQ(att_at_r, 0.0f, "Attenuation strictly 0 at radius R");

    float att_past_r = calculate_phys_attenuation(R + 5.0f, R);
    TEST_ASSERT_EQ(att_past_r, 0.0f, "Attenuation strictly 0 beyond radius R");

    return true;
}

static float calculate_spot_penumbra(float cos_angle, float spot_inner, float spot_outer) {
    if (cos_angle <= spot_outer) return 0.0f;
    if (cos_angle >= spot_inner) return 1.0f;
    float p = (cos_angle - spot_outer) / (spot_inner - spot_outer);
    return p * p;
}

[[nodiscard]]
bool test_spotlight_cone_penumbra(void) {
    /* Inner cone: 20 deg (cos ~= 0.93969)
     * Outer cone: 35 deg (cos ~= 0.81915)
     */
    const float inner = cosf(20.0f * (float)M_PI / 180.0f);
    const float outer = cosf(35.0f * (float)M_PI / 180.0f);

    /* Center axis (0 deg, cos = 1.0) -> full intensity */
    float pen_center = calculate_spot_penumbra(1.0f, inner, outer);
    TEST_ASSERT_EQ(pen_center, 1.0f, "Center axis spot attenuation is 1.0");

    /* Inside inner cone (15 deg) -> full intensity */
    float pen_inside = calculate_spot_penumbra(cosf(15.0f * (float)M_PI / 180.0f), inner, outer);
    TEST_ASSERT_EQ(pen_inside, 1.0f, "Inside inner cone spot attenuation is 1.0");

    /* In penumbra zone (27.5 deg) -> smooth transition (0 < pen < 1) */
    float pen_mid = calculate_spot_penumbra(cosf(27.5f * (float)M_PI / 180.0f), inner, outer);
    TEST_ASSERT(pen_mid > 0.1f && pen_mid < 0.9f, "Penumbra smoothly interpolates");

    /* Outside outer cone (45 deg) -> completely dark (0.0) */
    float pen_outside = calculate_spot_penumbra(cosf(45.0f * (float)M_PI / 180.0f), inner, outer);
    TEST_ASSERT_EQ(pen_outside, 0.0f, "Outside outer cone spot attenuation is 0.0");

    /* Backward direction (180 deg) -> completely dark (0.0) */
    float pen_back = calculate_spot_penumbra(-1.0f, inner, outer);
    TEST_ASSERT_EQ(pen_back, 0.0f, "Behind spotlight attenuation is 0.0");

    return true;
}

[[nodiscard]]
bool test_scene_lifecycle_and_bda_allocation(void) {
    khr_gfx_device_t dev = {};
    TEST_ASSERT(khr_gfx_device_init(&dev, (dev_t)0), "gfx device init");

    khr_bda_arena_t arena = {};
    TEST_ASSERT(khr_bda_arena_init(&dev, &arena, 128 * 1024), "bda arena init");

    khr_scene_t scene = {};
    TEST_ASSERT(khr_scene_init(&scene, &dev, &arena, 64, 8), "scene init");

    TEST_ASSERT_NOT_NULL(scene.instances, "instances host ptr");
    TEST_ASSERT(scene.instances_gpu != 0, "instances gpu addr");
    TEST_ASSERT_NOT_NULL(scene.culled_instances, "culled host ptr");
    TEST_ASSERT(scene.culled_instances_gpu != 0, "culled gpu addr");
    TEST_ASSERT_NOT_NULL(scene.lights, "lights host ptr");
    TEST_ASSERT(scene.lights_gpu != 0, "lights gpu addr");
    TEST_ASSERT_NOT_NULL(scene.draw_cmd, "draw_cmd host ptr");
    TEST_ASSERT(scene.draw_cmd_gpu != 0, "draw_cmd gpu addr");

    /* Generate procedural cube mesh */
    VkDeviceAddress cube_v = 0, cube_i = 0;
    uint32_t cube_vc = 0, cube_ic = 0;
    TEST_ASSERT(khr_scene_generate_cube(&arena, &cube_v, &cube_i, &cube_vc, &cube_ic), "gen cube");
    TEST_ASSERT_EQ(cube_vc, 8U, "cube has 8 verts");
    TEST_ASSERT_EQ(cube_ic, 36U, "cube has 36 indices");

    /* Register mesh 0 (Cube) */
    TEST_ASSERT(khr_scene_register_mesh(&scene, 0, cube_v, cube_i, cube_vc, cube_ic, 0.866f), "reg cube mesh");
    TEST_ASSERT_EQ(scene.mesh_count, 1U, "mesh count is 1");

    /* Add Instance 0: Gold Cube */
    khr_gpu_instance_t inst0 = {
        .position = { -2.0f, 0.0f, 0.0f },
        .radius = 0.866f,
        .rotation = { 0.0f, 0.0f, 0.0f, 1.0f },
        .scale = { 1.0f, 1.0f, 1.0f },
        .mesh_id = 0,
        .albedo = { 1.0f, 0.76f, 0.33f },
        .roughness = 0.2f,
        .metallic = 1.0f,
        .ao = 1.0f,
    };
    uint32_t i0 = khr_scene_add_instance(&scene, &inst0);
    TEST_ASSERT_EQ(i0, 0U, "instance 0 added");

    /* Add Instance 1: Dielectric Ruby Cube */
    khr_gpu_instance_t inst1 = {
        .position = { 2.0f, 0.0f, 0.0f },
        .radius = 0.866f,
        .rotation = { 0.0f, 0.0f, 0.0f, 1.0f },
        .scale = { 1.0f, 1.0f, 1.0f },
        .mesh_id = 0,
        .albedo = { 0.9f, 0.05f, 0.1f },
        .roughness = 0.4f,
        .metallic = 0.0f,
        .ao = 1.0f,
    };
    uint32_t i1 = khr_scene_add_instance(&scene, &inst1);
    TEST_ASSERT_EQ(i1, 1U, "instance 1 added");
    TEST_ASSERT_EQ(scene.instance_count, 2U, "instance count is 2");

    /* Add Physical Lights:
     * Light 0: Directional Sunlight
     * Light 1: Warm Point Light
     * Light 2: Cold Spotlight
     */
    khr_gpu_light_t sun = {
        .type = KHR_LIGHT_DIRECTIONAL,
        .direction = { -0.577f, -0.577f, -0.577f },
        .color = { 1.0f, 0.98f, 0.95f },
        .intensity = 2.0f,
    };
    uint32_t l0 = khr_scene_add_light(&scene, &sun);
    TEST_ASSERT_EQ(l0, 0U, "sun added");

    khr_gpu_light_t point = {
        .type = KHR_LIGHT_POINT,
        .position = { 0.0f, 3.0f, 0.0f },
        .range = 10.0f,
        .color = { 1.0f, 0.6f, 0.2f },
        .intensity = 15.0f,
    };
    uint32_t l1 = khr_scene_add_light(&scene, &point);
    TEST_ASSERT_EQ(l1, 1U, "point light added");

    khr_gpu_light_t spot = {
        .type = KHR_LIGHT_SPOT,
        .position = { 0.0f, 5.0f, 5.0f },
        .direction = { 0.0f, -0.707f, -0.707f },
        .range = 15.0f,
        .color = { 0.3f, 0.7f, 1.0f },
        .intensity = 25.0f,
        .spot_inner = cosf(15.0f * (float)M_PI / 180.0f),
        .spot_outer = cosf(30.0f * (float)M_PI / 180.0f),
    };
    uint32_t l2 = khr_scene_add_light(&scene, &spot);
    TEST_ASSERT_EQ(l2, 2U, "spot light added");
    TEST_ASSERT_EQ(scene.light_count, 3U, "light count is 3");

    /* Test TRS update */
    float new_pos[3] = { -2.5f, 0.5f, 0.0f };
    khr_scene_update_instance_trs(&scene, 0, new_pos, nullptr, nullptr);
    TEST_ASSERT(fabsf(scene.instances[0].position[0] - (-2.5f)) < 1.0e-5f, "instance 0 position updated");

    /* Test Light update */
    point.intensity = 20.0f;
    khr_scene_update_light(&scene, 1, &point);
    TEST_ASSERT(fabsf(scene.lights[1].intensity - 20.0f) < 1.0e-5f, "light 1 intensity updated");

    /* Cleanup */
    khr_scene_destroy(&scene);
    khr_bda_arena_destroy(&dev, &arena);
    khr_gfx_device_destroy(&dev);
    return true;
}

[[nodiscard]]
bool test_multi_mesh_compute_culling_and_indirect_dispatch(void) {
    khr_gfx_device_t dev = {};
    TEST_ASSERT(khr_gfx_device_init(&dev, (dev_t)0), "gfx device init");

    khr_cull_pipeline_t cull_pipe = {};
    TEST_ASSERT(khr_cull_pipeline_init(&cull_pipe, &dev), "cull pipe init");

    khr_bda_arena_t arena = {};
    TEST_ASSERT(khr_bda_arena_init(&dev, &arena, 128 * 1024), "bda arena init");

    khr_scene_t scene = {};
    TEST_ASSERT(khr_scene_init(&scene, &dev, &arena, 64, 8), "scene init");

    /* Generate and register Cube mesh */
    VkDeviceAddress cube_v = 0, cube_i = 0;
    uint32_t cube_vc = 0, cube_ic = 0;
    TEST_ASSERT(khr_scene_generate_cube(&arena, &cube_v, &cube_i, &cube_vc, &cube_ic), "gen cube");
    TEST_ASSERT(khr_scene_register_mesh(&scene, 0, cube_v, cube_i, cube_vc, cube_ic, 0.866f), "reg mesh");

    /* Instances:
     * Inst 0: at (0, 0, 0) -> in front of camera at (0, 0, 5) -> VISIBLE
     * Inst 1: at (0, 0, 50) -> behind camera -> CULLED
     * Inst 2: at (1, 1, 0) -> in front of camera -> VISIBLE
     */
    khr_gpu_instance_t inst0 = { .position = { 0.0f, 0.0f, 0.0f }, .radius = 0.866f, .rotation = { 0, 0, 0, 1 }, .scale = { 1, 1, 1 }, .mesh_id = 0 };
    khr_gpu_instance_t inst1 = { .position = { 0.0f, 0.0f, 50.0f }, .radius = 0.866f, .rotation = { 0, 0, 0, 1 }, .scale = { 1, 1, 1 }, .mesh_id = 0 };
    khr_gpu_instance_t inst2 = { .position = { 1.0f, 1.0f, 0.0f }, .radius = 0.866f, .rotation = { 0, 0, 0, 1 }, .scale = { 1, 1, 1 }, .mesh_id = 0 };

    khr_scene_add_instance(&scene, &inst0);
    khr_scene_add_instance(&scene, &inst1);
    khr_scene_add_instance(&scene, &inst2);

    /* Camera at (0, 0, 5) looking at (0, 0, 0) */
    khr_camera_t cam = {};
    khr_camera_init(&cam, 1.04719755f, 1.0f, 0.1f);
    khr_camera_look_at(&cam, (float[]){ 0.0f, 0.0f, 5.0f }, (float[]){ 0.0f, 0.0f, 0.0f }, (float[]){ 0.0f, 1.0f, 0.0f });

    khr_cull_push_t cull_push = {};
    khr_scene_prepare_cull_push(&scene, &cam, &cull_push);

    /* Command buffer */
    VkCommandBufferAllocateInfo cb_ai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = dev.pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    TEST_ASSERT_EQ(vkAllocateCommandBuffers(dev.device, &cb_ai, &cmd), VK_SUCCESS, "alloc cmd");

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    TEST_ASSERT_EQ(vkBeginCommandBuffer(cmd, &begin_info), VK_SUCCESS, "begin cmd");

    khr_cull_pipeline_dispatch(&cull_pipe, cmd, &cull_push);

    /* Barrier: Compute write -> Host read */
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

    dev.acquire_point++;
    uint64_t sig_val = dev.acquire_point;
    VkTimelineSemaphoreSubmitInfo timeline_info = {
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .signalSemaphoreValueCount = 1,
        .pSignalSemaphoreValues = &sig_val,
    };
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = &timeline_info,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &dev.acquire_sem,
    };
    TEST_ASSERT_EQ(vkQueueSubmit(dev.gfx_queue, 1, &si, VK_NULL_HANDLE), VK_SUCCESS, "queue submit");

    VkSemaphoreWaitInfo wi = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores = &dev.acquire_sem,
        .pValues = &sig_val,
    };
    TEST_ASSERT_EQ(vkWaitSemaphores(dev.device, &wi, 5'000'000'000ULL), VK_SUCCESS, "wait semaphore");

    /* Exactly 2 instances must be visible */
    TEST_ASSERT_EQ(scene.draw_cmd->instanceCount, 2U, "GPU compute culling compacted exactly 2 instances");
    TEST_ASSERT_EQ(*scene.draw_count, 2U, "Atomic count must equal 2");

    /* Cleanup */
    vkFreeCommandBuffers(dev.device, dev.pool, 1, &cmd);
    khr_scene_destroy(&scene);
    khr_bda_arena_destroy(&dev, &arena);
    khr_cull_pipeline_destroy(&cull_pipe);
    khr_gfx_device_destroy(&dev);
    return true;
}

static uint32_t find_mem_type(VkPhysicalDevice phy, uint32_t type_bits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(phy, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((type_bits & (1U << i)) && (mem_props.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if (type_bits & (1U << i)) {
            return i;
        }
    }
    return 0;
}

[[nodiscard]]
bool test_multi_light_gpu_pbr_shading(void) {
    khr_gfx_device_t dev = {};
    TEST_ASSERT(khr_gfx_device_init(&dev, (dev_t)0), "gfx device init");

    khr_cull_pipeline_t cull_pipe = {};
    TEST_ASSERT(khr_cull_pipeline_init(&cull_pipe, &dev), "cull pipe init");

    khr_mesh_instanced_pipeline_t inst_pipe = {};
    TEST_ASSERT(khr_mesh_instanced_pipeline_init(&inst_pipe, &dev, VK_FORMAT_B8G8R8A8_UNORM, nullptr), "mesh instanced pipe init");

    khr_bda_arena_t arena = {};
    TEST_ASSERT(khr_bda_arena_init(&dev, &arena, 128 * 1024), "bda arena init");

    khr_scene_t scene = {};
    TEST_ASSERT(khr_scene_init(&scene, &dev, &arena, 16, 4), "scene init");

    /* Generate and register Cube */
    VkDeviceAddress cube_v = 0, cube_i = 0;
    uint32_t cube_vc = 0, cube_ic = 0;
    TEST_ASSERT(khr_scene_generate_cube(&arena, &cube_v, &cube_i, &cube_vc, &cube_ic), "gen cube");
    TEST_ASSERT(khr_scene_register_mesh(&scene, 0, cube_v, cube_i, cube_vc, cube_ic, 0.866f), "reg cube");

    /* Add 2 instances with distinct materials */
    khr_gpu_instance_t inst0 = {
        .position = { -1.2f, 0.0f, 0.0f },
        .radius = 0.866f,
        .rotation = { 0.0f, 0.0f, 0.0f, 1.0f },
        .scale = { 1.0f, 1.0f, 1.0f },
        .mesh_id = 0,
        .albedo = { 1.0f, 0.76f, 0.33f }, /* Gold */
        .roughness = 0.2f,
        .metallic = 0.95f,
        .ao = 1.0f,
        .albedo_tex_id = UINT32_MAX,
        .normal_tex_id = UINT32_MAX,
    };
    khr_gpu_instance_t inst1 = {
        .position = { 1.2f, 0.0f, 0.0f },
        .radius = 0.866f,
        .rotation = { 0.0f, 0.0f, 0.0f, 1.0f },
        .scale = { 1.0f, 1.0f, 1.0f },
        .mesh_id = 0,
        .albedo = { 0.2f, 0.5f, 0.9f }, /* Blue plastic */
        .roughness = 0.6f,
        .metallic = 0.0f,
        .ao = 1.0f,
        .albedo_tex_id = UINT32_MAX,
        .normal_tex_id = UINT32_MAX,
    };
    khr_scene_add_instance(&scene, &inst0);
    khr_scene_add_instance(&scene, &inst1);

    /* Add Real Physical Lights:
     * Light 0: Directional Sun
     * Light 1: Warm Point light with inverse-square falloff
     * Light 2: Spot light with angular penumbra
     */
    khr_gpu_light_t sun = {
        .type = KHR_LIGHT_DIRECTIONAL,
        .direction = { -0.577f, -0.577f, -0.577f },
        .color = { 1.0f, 0.95f, 0.9f },
        .intensity = 1.5f,
    };
    khr_gpu_light_t point = {
        .type = KHR_LIGHT_POINT,
        .position = { 0.0f, 2.0f, 1.0f },
        .range = 8.0f,
        .color = { 1.0f, 0.5f, 0.1f },
        .intensity = 12.0f,
    };
    khr_gpu_light_t spot = {
        .type = KHR_LIGHT_SPOT,
        .position = { 0.0f, 3.0f, 3.0f },
        .direction = { 0.0f, -0.707f, -0.707f },
        .range = 10.0f,
        .color = { 0.2f, 0.8f, 1.0f },
        .intensity = 20.0f,
        .spot_inner = cosf(20.0f * (float)M_PI / 180.0f),
        .spot_outer = cosf(35.0f * (float)M_PI / 180.0f),
    };
    khr_scene_add_light(&scene, &sun);
    khr_scene_add_light(&scene, &point);
    khr_scene_add_light(&scene, &spot);

    /* Camera */
    khr_camera_t cam = {};
    khr_camera_init(&cam, 1.04719755f, 1.0f, 0.1f);
    khr_camera_look_at(&cam, (float[]){ 0.0f, 0.0f, 6.0f }, (float[]){ 0.0f, 0.0f, 0.0f }, (float[]){ 0.0f, 1.0f, 0.0f });

    /* Prepare Push Constants */
    khr_cull_push_t cull_push = {};
    khr_scene_prepare_cull_push(&scene, &cam, &cull_push);

    khr_mesh_instanced_push_t mesh_push = {};
    khr_scene_prepare_mesh_push(&scene, 0, &cam, &mesh_push);

    /* Create Render Target and Reversed-Z Depth */
    VkImage color_image = VK_NULL_HANDLE;
    VkDeviceMemory color_mem = VK_NULL_HANDLE;
    VkImageView color_view = VK_NULL_HANDLE;

    VkImageCreateInfo img_ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .extent = { 256, 256, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    TEST_ASSERT_EQ(vkCreateImage(dev.device, &img_ci, nullptr, &color_image), VK_SUCCESS, "create color img");

    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(dev.device, color_image, &mem_reqs);
    uint32_t mem_type_idx = find_mem_type(dev.phy, mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo mem_ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_reqs.size,
        .memoryTypeIndex = mem_type_idx,
    };
    TEST_ASSERT_EQ(vkAllocateMemory(dev.device, &mem_ai, nullptr, &color_mem), VK_SUCCESS, "alloc color mem");
    TEST_ASSERT_EQ(vkBindImageMemory(dev.device, color_image, color_mem, 0), VK_SUCCESS, "bind color mem");

    VkImageViewCreateInfo view_ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = color_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
    };
    TEST_ASSERT_EQ(vkCreateImageView(dev.device, &view_ci, nullptr, &color_view), VK_SUCCESS, "create color view");

    khr_depth_target_t depth_target = {};
    TEST_ASSERT(khr_depth_target_create(&dev, &depth_target, 256, 256, VK_SAMPLE_COUNT_1_BIT, VK_FORMAT_UNDEFINED), "depth init");

    VkImageAspectFlags depth_aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (depth_target.format == VK_FORMAT_D24_UNORM_S8_UINT || depth_target.format == VK_FORMAT_D32_SFLOAT_S8_UINT) {
        depth_aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    }

    /* Command Buffer */
    VkCommandBufferAllocateInfo cb_ai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = dev.pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    TEST_ASSERT_EQ(vkAllocateCommandBuffers(dev.device, &cb_ai, &cmd), VK_SUCCESS, "alloc cmd");

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    TEST_ASSERT_EQ(vkBeginCommandBuffer(cmd, &begin_info), VK_SUCCESS, "begin cmd");

    /* 1. Run GPU compute culling */
    khr_cull_pipeline_dispatch(&cull_pipe, cmd, &cull_push);

    /* Compute write -> Indirect read & Shader read barrier */
    VkMemoryBarrier2 mb = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
    };
    VkDependencyInfo dep = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &mb,
    };
    vkCmdPipelineBarrier2(cmd, &dep);

    /* 2. Transition images */
    VkImageMemoryBarrier2 img_barriers[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
            .srcAccessMask = 0,
            .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .image = color_image,
            .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
            .srcAccessMask = 0,
            .dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
            .dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            .image = depth_target.image,
            .subresourceRange = { .aspectMask = depth_aspect, .levelCount = 1, .layerCount = 1 },
        }
    };
    VkDependencyInfo img_dep = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 2,
        .pImageMemoryBarriers = img_barriers,
    };
    vkCmdPipelineBarrier2(cmd, &img_dep);

    /* 3. Begin dynamic rendering with Reversed-Z clear */
    VkRenderingAttachmentInfo color_att = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = color_view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = { .color = { .float32 = { 0.05f, 0.05f, 0.05f, 1.0f } } },
    };
    VkRenderingAttachmentInfo depth_att = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = depth_target.view,
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .clearValue = { .depthStencil = { .depth = 0.0f } }, /* Reversed-Z far = 0.0 */
    };
    VkRenderingInfo rend_info = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = { .offset = { 0, 0 }, .extent = { 256, 256 } },
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_att,
        .pDepthAttachment = &depth_att,
    };
    vkCmdBeginRendering(cmd, &rend_info);

    VkViewport vp = { 0.0f, 0.0f, 256.0f, 256.0f, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { 256, 256 } };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);

    /* Draw indirect with GPU-culled instance count and multi-light PBR */
    khr_mesh_instanced_draw_indirect(&inst_pipe, cmd, &mesh_push, arena.buffer,
                                     (VkDeviceSize)((uintptr_t)scene.draw_cmd - (uintptr_t)arena.host_ptr),
                                     1, sizeof(khr_draw_indirect_cmd_t));

    vkCmdEndRendering(cmd);

    TEST_ASSERT_EQ(vkEndCommandBuffer(cmd), VK_SUCCESS, "end cmd");

    /* Submit and wait */
    dev.acquire_point++;
    uint64_t sig_val = dev.acquire_point;
    VkTimelineSemaphoreSubmitInfo timeline_info = {
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .signalSemaphoreValueCount = 1,
        .pSignalSemaphoreValues = &sig_val,
    };
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = &timeline_info,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &dev.acquire_sem,
    };
    TEST_ASSERT_EQ(vkQueueSubmit(dev.gfx_queue, 1, &si, VK_NULL_HANDLE), VK_SUCCESS, "queue submit");

    VkSemaphoreWaitInfo wi = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores = &dev.acquire_sem,
        .pValues = &sig_val,
    };
    TEST_ASSERT_EQ(vkWaitSemaphores(dev.device, &wi, 5'000'000'000ULL), VK_SUCCESS, "wait semaphore");

    /* Verify that both instances rendered */
    TEST_ASSERT_EQ(scene.draw_cmd->instanceCount, 2U, "Both instances visible");

    /* Cleanup */
    vkFreeCommandBuffers(dev.device, dev.pool, 1, &cmd);
    khr_depth_target_destroy(&dev, &depth_target);
    vkDestroyImageView(dev.device, color_view, nullptr);
    vkDestroyImage(dev.device, color_image, nullptr);
    vkFreeMemory(dev.device, color_mem, nullptr);
    khr_scene_destroy(&scene);
    khr_bda_arena_destroy(&dev, &arena);
    khr_mesh_instanced_pipeline_destroy(&inst_pipe);
    khr_cull_pipeline_destroy(&cull_pipe);
    khr_gfx_device_destroy(&dev);
    return true;
}

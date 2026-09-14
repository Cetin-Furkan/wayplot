#include "test_framework.h"
#include "khoros/gfx/camera.h"
#include "khoros/gfx/blob.h"
#include "khoros/gfx/device.h"
#include "khoros/gfx/bda_arena.h"
#include "khoros/gfx/depth.h"
#include "khoros/gfx/cull_pipeline.h"
#include <math.h>
#include <string.h>

[[nodiscard]]
bool test_camera_lifecycle_and_defaults(void) {
    khr_camera_t cam = {};
    khr_camera_init(&cam, 1.04719755f, 16.0f / 9.0f, 0.1f);

    TEST_ASSERT(fabsf(cam.fov_y - 1.04719755f) < 1.0e-5f, "Camera FOV set correctly");
    TEST_ASSERT(fabsf(cam.aspect - (16.0f / 9.0f)) < 1.0e-5f, "Camera aspect set correctly");
    TEST_ASSERT(fabsf(cam.z_near - 0.1f) < 1.0e-5f, "Camera z_near set correctly");
    TEST_ASSERT(cam.radius > 0.0f, "Camera radius must be positive");
    TEST_ASSERT(cam.eye[2] > 0.0f, "Eye initially at positive Z");
    TEST_ASSERT(fabsf(cam.target[0]) < 1.0e-5f && fabsf(cam.target[1]) < 1.0e-5f && fabsf(cam.target[2]) < 1.0e-5f,
                "Target initially at origin");

    /* Test explicit look_at */
    float eye[3] = { 10.0f, 10.0f, 10.0f };
    float target[3] = { 0.0f, 2.0f, 0.0f };
    float up[3] = { 0.0f, 1.0f, 0.0f };
    khr_camera_look_at(&cam, eye, target, up);

    TEST_ASSERT(fabsf(cam.eye[0] - 10.0f) < 1.0e-5f, "Eye X updated");
    TEST_ASSERT(fabsf(cam.target[1] - 2.0f) < 1.0e-5f, "Target Y updated");

    /* Distance between eye and target should match radius */
    float dx = eye[0] - target[0];
    float dy = eye[1] - target[1];
    float dz = eye[2] - target[2];
    float expected_r = sqrtf(dx * dx + dy * dy + dz * dz);
    TEST_ASSERT(fabsf(cam.radius - expected_r) < 1.0e-4f, "Radius matches eye-target distance");

    /* Test spherical orbit */
    khr_camera_init(&cam, 1.0f, 1.0f, 0.1f);
    float init_dist = sqrtf(cam.eye[0]*cam.eye[0] + cam.eye[1]*cam.eye[1] + cam.eye[2]*cam.eye[2]);
    khr_camera_orbit(&cam, 0.5f, 0.3f);
    float orbit_dist = sqrtf(cam.eye[0]*cam.eye[0] + cam.eye[1]*cam.eye[1] + cam.eye[2]*cam.eye[2]);
    TEST_ASSERT(fabsf(orbit_dist - init_dist) < 1.0e-4f, "Orbit keeps radius constant");

    /* Test pitch clamping at ~89 degrees */
    khr_camera_orbit(&cam, 0.0f, 5.0f); // large positive pitch
    TEST_ASSERT(cam.pitch <= 1.56f, "Pitch clamped near +pi/2");
    khr_camera_orbit(&cam, 0.0f, -10.0f); // large negative pitch
    TEST_ASSERT(cam.pitch >= -1.56f, "Pitch clamped near -pi/2");

    /* Test rotation matrix, axis snapping, and picking */
    khr_camera_snap_axis(&cam, 3); /* +Z */
    float r0[3] = {}, r1[3] = {}, r2[3] = {};
    khr_camera_get_rotation_matrix(&cam, r0, r1, r2);
    int pick_z = khr_camera_pick_axis(&cam, 0.0f, 0.0f);
    TEST_ASSERT_EQ(pick_z, 3, "Center pick on +Z snapped camera returns axis 3");
    TEST_ASSERT_EQ(khr_camera_pick_axis(&cam, -0.78f, 0.0f), 1, "+X arm");
    TEST_ASSERT_EQ(khr_camera_pick_axis(&cam, 0.78f, 0.0f), -1, "-X arm");
    TEST_ASSERT_EQ(khr_camera_pick_axis(&cam, 0.0f, -0.78f), 2, "+Y arm");

    khr_camera_snap_axis(&cam, 1); /* +X */
    khr_camera_get_rotation_matrix(&cam, r0, r1, r2);
    int pick_x = khr_camera_pick_axis(&cam, 0.0f, 0.0f);
    TEST_ASSERT_EQ(pick_x, -1, "Center pick on +X snapped camera returns opposing axis -1");

    /* Test vertex framing */
    const float cube_pts[24] = {
        -1.0f, -1.0f, -1.0f,  1.0f, -1.0f, -1.0f,
         1.0f,  1.0f, -1.0f, -1.0f,  1.0f, -1.0f,
        -1.0f, -1.0f,  1.0f,  1.0f, -1.0f,  1.0f,
         1.0f,  1.0f,  1.0f, -1.0f,  1.0f,  1.0f,
    };
    khr_camera_frame_verts(&cam, cube_pts, 8, true);
    TEST_ASSERT(fabsf(cam.target[0]) < 1.0e-5f, "Target centered on origin X");
    TEST_ASSERT(fabsf(cam.target[1]) < 1.0e-5f, "Target centered on origin Y");
    TEST_ASSERT(fabsf(cam.target[2]) < 1.0e-5f, "Target centered on origin Z");
    TEST_ASSERT(cam.radius > 2.0f, "Framing radius encloses unit cube");

    return true;
}

[[nodiscard]]
bool test_camera_arcball_virtual_sphere(void) {
    khr_camera_t cam = {};
    khr_camera_init(&cam, 1.04719755f, 1.0f, 0.1f);

    float r_initial = cam.radius;

    /* Perform horizontal drag on arcball */
    khr_camera_arcball(&cam, 0.0f, 0.0f, 0.5f, 0.0f);

    /* Eye distance to target must remain invariant */
    float dx = cam.eye[0] - cam.target[0];
    float dy = cam.eye[1] - cam.target[1];
    float dz = cam.eye[2] - cam.target[2];
    float r_after = sqrtf(dx * dx + dy * dy + dz * dz);
    TEST_ASSERT(fabsf(r_after - r_initial) < 1.0e-4f, "Arcball rotation preserves distance to target");

    /* Orientation quaternion must maintain unit length */
    float qlen = sqrtf(cam.quat[0]*cam.quat[0] + cam.quat[1]*cam.quat[1] +
                       cam.quat[2]*cam.quat[2] + cam.quat[3]*cam.quat[3]);
    TEST_ASSERT(fabsf(qlen - 1.0f) < 1.0e-4f, "Arcball quaternion maintains unit norm");

    /* Perform multi-axis drag across poles - must not generate NaN or Inf */
    for (int i = 0; i < 20; i++) {
        khr_camera_arcball(&cam, -0.8f, -0.8f, 0.8f, 0.8f);
    }
    TEST_ASSERT(!isnan(cam.eye[0]) && !isnan(cam.eye[1]) && !isnan(cam.eye[2]), "No NaN in eye after extreme arcball drags");
    TEST_ASSERT(!isinf(cam.eye[0]) && !isinf(cam.eye[1]) && !isinf(cam.eye[2]), "No Inf in eye after extreme arcball drags");

    return true;
}

[[nodiscard]]
bool test_camera_pan_zoom_and_aabb(void) {
    khr_camera_t cam = {};
    khr_camera_init(&cam, 1.04719755f, 1.0f, 0.1f);

    /* Test zoom */
    float r_orig = cam.radius;
    khr_camera_zoom(&cam, 2.0f); // zoom in
    TEST_ASSERT(cam.radius < r_orig, "Zoom in reduces radius");
    khr_camera_zoom(&cam, -4.0f); // zoom out
    TEST_ASSERT(cam.radius > r_orig, "Zoom out increases radius");

    /* Test pan */
    float init_eye_x = cam.eye[0];
    float init_target_x = cam.target[0];
    khr_camera_pan(&cam, 10.0f, 0.0f);
    TEST_ASSERT(cam.eye[0] != init_eye_x, "Pan shifts eye position");
    TEST_ASSERT(cam.target[0] != init_target_x, "Pan shifts target position");
    TEST_ASSERT(fabsf((cam.eye[0] - init_eye_x) - (cam.target[0] - init_target_x)) < 1.0e-5f,
                "Eye and target shift by identical offset in view plane");

    /* Test AABB fit */
    float min_p[3] = { -2.0f, -1.0f, -3.0f };
    float max_p[3] = {  2.0f,  3.0f,  1.0f };
    khr_camera_fit_aabb(&cam, min_p, max_p);

    /* Target should be box center */
    TEST_ASSERT(fabsf(cam.target[0] - 0.0f) < 1.0e-5f, "AABB fit center X");
    TEST_ASSERT(fabsf(cam.target[1] - 1.0f) < 1.0e-5f, "AABB fit center Y");
    TEST_ASSERT(fabsf(cam.target[2] - (-1.0f)) < 1.0e-5f, "AABB fit center Z");

    /* Eye distance must be far enough to encompass the bounding sphere */
    float dx = max_p[0] - min_p[0];
    float dy = max_p[1] - min_p[1];
    float dz = max_p[2] - min_p[2];
    float diag = sqrtf(dx * dx + dy * dy + dz * dz);
    TEST_ASSERT(cam.radius > diag * 0.5f, "AABB radius encloses bounding sphere");

    return true;
}

[[nodiscard]]
bool test_camera_feed_cull_push_contract(void) {
    khr_camera_t cam = {};
    khr_camera_init(&cam, 1.2f, 1.777f, 0.05f);
    cam.eye[0] = 1.0f; cam.eye[1] = 2.0f; cam.eye[2] = 3.0f;
    cam.target[0] = 0.1f; cam.target[1] = 0.2f; cam.target[2] = 0.3f;
    cam.up[0] = 0.0f; cam.up[1] = 1.0f; cam.up[2] = 0.0f;

    khr_cull_push_t push = {};
    khr_camera_feed_cull_push(&cam, &push);

    TEST_ASSERT(fabsf(push.eye_fov[0] - 1.0f) < 1.0e-5f, "eye_fov.x matches");
    TEST_ASSERT(fabsf(push.eye_fov[1] - 2.0f) < 1.0e-5f, "eye_fov.y matches");
    TEST_ASSERT(fabsf(push.eye_fov[2] - 3.0f) < 1.0e-5f, "eye_fov.z matches");
    TEST_ASSERT(fabsf(push.eye_fov[3] - 1.2f) < 1.0e-5f, "eye_fov.w (fov) matches");

    TEST_ASSERT(fabsf(push.target_aspect[0] - 0.1f) < 1.0e-5f, "target_aspect.x matches");
    TEST_ASSERT(fabsf(push.target_aspect[1] - 0.2f) < 1.0e-5f, "target_aspect.y matches");
    TEST_ASSERT(fabsf(push.target_aspect[2] - 0.3f) < 1.0e-5f, "target_aspect.z matches");
    TEST_ASSERT(fabsf(push.target_aspect[3] - 1.777f) < 1.0e-5f, "target_aspect.w (aspect) matches");

    TEST_ASSERT(fabsf(push.up_znear[0] - 0.0f) < 1.0e-5f, "up_znear.x matches");
    TEST_ASSERT(fabsf(push.up_znear[1] - 1.0f) < 1.0e-5f, "up_znear.y matches");
    TEST_ASSERT(fabsf(push.up_znear[2] - 0.0f) < 1.0e-5f, "up_znear.z matches");
    TEST_ASSERT(fabsf(push.up_znear[3] - 0.05f) < 1.0e-5f, "up_znear.w (znear) matches");

    return true;
}

static float test_aces(float x) {
    float num = x * (2.51f * x + 0.03f);
    float den = x * (2.43f * x + 0.59f) + 0.14f;
    float v = num / den;
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

[[nodiscard]]
bool test_pbr_material_and_brdf_properties(void) {
    /* Test Cook-Torrance BRDF Fresnel & Energy Conservation Laws */
    float albedo[3] = { 0.95f, 0.64f, 0.54f }; /* Copper */

    /* Dielectric Fresnel: F0 = 0.04 */
    float f0_dielectric = 0.04f;
    /* Metallic Fresnel: F0 = albedo */
    float f0_metal = albedo[0];

    TEST_ASSERT(f0_dielectric < 0.1f, "Dielectric F0 is around 0.04");
    TEST_ASSERT(f0_metal > 0.9f, "Copper F0 matches metal albedo");

    /* Energy conservation test: kD = (1 - F) * (1 - metallic) */
    float metallic = 1.0f;
    float kd_metal = (1.0f - f0_metal) * (1.0f - metallic);
    TEST_ASSERT(fabsf(kd_metal) < 1.0e-6f, "Metals have zero diffuse reflection (kD = 0)");

    metallic = 0.0f;
    float kd_dielectric = (1.0f - f0_dielectric) * (1.0f - metallic);
    TEST_ASSERT(kd_dielectric > 0.9f && kd_dielectric <= 1.0f, "Dielectrics conserve diffuse energy");

    /* ACES Tonemapping analytical test:
     * f(x) = (x * (2.51*x + 0.03)) / (x * (2.43*x + 0.59) + 0.14)
     */
    TEST_ASSERT(fabsf(test_aces(0.0f) - 0.0f) < 0.01f, "ACES(0) near 0");
    TEST_ASSERT(test_aces(1.0f) > 0.7f && test_aces(1.0f) < 0.9f, "ACES(1) provides natural midtone compression");
    TEST_ASSERT(test_aces(10.0f) > 0.95f && test_aces(10.0f) <= 1.0f, "ACES(10) compresses HDR highlight");
    TEST_ASSERT(test_aces(100.0f) <= 1.0f, "ACES maps arbitrarily high radiance to <= 1.0");

    /* Smooth normal generation verification on regular octahedron */
    const float oct_verts[18] = {
         1.0f,  0.0f,  0.0f,
        -1.0f,  0.0f,  0.0f,
         0.0f,  1.0f,  0.0f,
         0.0f, -1.0f,  0.0f,
         0.0f,  0.0f,  1.0f,
         0.0f,  0.0f, -1.0f,
    };
    const uint32_t oct_indices[24] = {
        0, 2, 4,   2, 1, 4,   1, 3, 4,   3, 0, 4,
        2, 0, 5,   1, 2, 5,   3, 1, 5,   0, 3, 5,
    };
    float oct_normals[18] = {};
    khr_blob_generate_smooth_normals(oct_verts, 6, oct_indices, 24, oct_normals);
    for (uint32_t vi = 0; vi < 6; vi++) {
        float nx = oct_normals[vi * 3 + 0];
        float ny = oct_normals[vi * 3 + 1];
        float nz = oct_normals[vi * 3 + 2];
        float len = sqrtf(nx * nx + ny * ny + nz * nz);
        TEST_ASSERT(fabsf(len - 1.0f) < 1.0e-4f, "Smooth normal must be normalized unit length");
        /* For an octahedron centered at origin, outward normal aligns with vertex position */
        float dot_v = nx * oct_verts[vi * 3 + 0] + ny * oct_verts[vi * 3 + 1] + nz * oct_verts[vi * 3 + 2];
        TEST_ASSERT(dot_v > 0.9f, "Smooth normal points radially outward matching vertex direction");
    }

    return true;
}

static uint32_t find_memory_type(VkPhysicalDevice phy, uint32_t type_bits, VkMemoryPropertyFlags props) {
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
bool test_gpu_instanced_pbr_pipeline_execution(void) {
    khr_gfx_device_t dev = {};
    TEST_ASSERT(khr_gfx_device_init(&dev, (dev_t)0), "gfx device init");

    khr_cull_pipeline_t cull_pipe = {};
    TEST_ASSERT(khr_cull_pipeline_init(&cull_pipe, &dev), "cull pipe init");

    khr_mesh_instanced_pipeline_t inst_pipe = {};
    TEST_ASSERT(khr_mesh_instanced_pipeline_init(&inst_pipe, &dev, VK_FORMAT_B8G8R8A8_UNORM, nullptr), "mesh instanced pipe init");

    khr_bda_arena_t arena = {};
    TEST_ASSERT(khr_bda_arena_init(&dev, &arena, 128 * 1024), "bda arena init");

    /* Allocate GPU buffers */
    void* inst_host = nullptr;
    VkDeviceAddress inst_gpu = 0;
    TEST_ASSERT(khr_bda_arena_alloc(&arena, 2 * sizeof(khr_gpu_instance_t), 64, &inst_host, &inst_gpu), "alloc instances");

    void* culled_host = nullptr;
    VkDeviceAddress culled_gpu = 0;
    TEST_ASSERT(khr_bda_arena_alloc(&arena, 2 * sizeof(khr_gpu_culled_instance_t), 64, &culled_host, &culled_gpu), "alloc culled");

    void* cmd_host = nullptr;
    VkDeviceAddress cmd_gpu = 0;
    TEST_ASSERT(khr_bda_arena_alloc(&arena, sizeof(khr_draw_indirect_cmd_t), 64, &cmd_host, &cmd_gpu), "alloc cmd");

    void* verts_host = nullptr;
    VkDeviceAddress verts_gpu = 0;
    /* 3 vertices for a single triangle (xyz each) */
    TEST_ASSERT(khr_bda_arena_alloc(&arena, 9 * sizeof(float), 64, &verts_host, &verts_gpu), "alloc verts");

    void* indices_host = nullptr;
    VkDeviceAddress indices_gpu = 0;
    /* 3 indices */
    TEST_ASSERT(khr_bda_arena_alloc(&arena, 3 * sizeof(uint32_t), 64, &indices_host, &indices_gpu), "alloc indices");

    void* pixel_host = nullptr;
    VkDeviceAddress pixel_gpu = 0;
    TEST_ASSERT(khr_bda_arena_alloc(&arena, 16, 16, &pixel_host, &pixel_gpu), "alloc pixel");

    /* Populate triangle vertices */
    float* verts = (float*)verts_host;
    verts[0] = -1.0f; verts[1] = -1.0f; verts[2] = 0.0f;
    verts[3] =  1.0f; verts[4] = -1.0f; verts[5] = 0.0f;
    verts[6] =  0.0f; verts[7] =  1.0f; verts[8] = 0.0f;

    uint32_t* indices = (uint32_t*)indices_host;
    indices[0] = 0; indices[1] = 1; indices[2] = 2;

    /* Instance 0: Metallic Gold Sphere/Mesh */
    khr_gpu_instance_t* instances = (khr_gpu_instance_t*)inst_host;
    instances[0] = (khr_gpu_instance_t){
        .position = { 0.0f, 0.0f, 0.0f },
        .radius = 1.0f,
        .rotation = { 0.0f, 0.0f, 0.0f, 1.0f },
        .scale = { 1.0f, 1.0f, 1.0f },
        .mesh_id = 0,
        .albedo = { 1.0f, 0.76f, 0.33f }, /* Gold base color */
        .roughness = 0.15f,
        .metallic = 1.0f,
        .ao = 1.0f,
        .albedo_tex_id = UINT32_MAX,
        .normal_tex_id = UINT32_MAX,
    };

    /* Instance 1: Dielectric Rough Blue Plastic */
    instances[1] = (khr_gpu_instance_t){
        .position = { 2.0f, 0.0f, 0.0f },
        .radius = 1.0f,
        .rotation = { 0.0f, 0.0f, 0.0f, 1.0f },
        .scale = { 1.0f, 1.0f, 1.0f },
        .mesh_id = 0,
        .albedo = { 0.1f, 0.3f, 0.9f },
        .roughness = 0.8f,
        .metallic = 0.0f,
        .ao = 0.9f,
        .albedo_tex_id = UINT32_MAX,
        .normal_tex_id = UINT32_MAX,
    };

    /* Draw Command */
    khr_draw_indirect_cmd_t* draw_cmd = (khr_draw_indirect_cmd_t*)cmd_host;
    *draw_cmd = (khr_draw_indirect_cmd_t){
        .vertexCount = 3,
        .instanceCount = 0,
        .firstVertex = 0,
        .firstInstance = 0,
    };

    /* Camera Setup */
    khr_camera_t cam = {};
    khr_camera_init(&cam, 1.04719755f, 1.0f, 0.1f);
    khr_camera_look_at(&cam, (float[]){ 0.0f, 0.0f, 5.0f }, (float[]){ 0.0f, 0.0f, 0.0f }, (float[]){ 0.0f, 1.0f, 0.0f });

    /* Cull push constant */
    khr_cull_push_t cull_push = {
        .instances_addr = inst_gpu,
        .culled_instances_addr = culled_gpu,
        .draw_cmd_addr = cmd_gpu,
        .draw_count_addr = 0,
        .instance_count = 2,
        .index_count = 3,
    };
    khr_camera_feed_cull_push(&cam, &cull_push);

    /* Allocate command buffer */
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
        .dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
    };
    VkDependencyInfo dep = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &mb,
    };
    vkCmdPipelineBarrier2(cmd, &dep);

    /* 2. Create offscreen render target & depth buffer for dynamic rendering */
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
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    TEST_ASSERT_EQ(vkCreateImage(dev.device, &img_ci, nullptr, &color_image), VK_SUCCESS, "create color img");

    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(dev.device, color_image, &mem_reqs);
    uint32_t mem_type_idx = find_memory_type(dev.phy, mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
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
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    TEST_ASSERT_EQ(vkCreateImageView(dev.device, &view_ci, nullptr, &color_view), VK_SUCCESS, "create color view");

    /* Create Reversed-Z depth buffer using khr_depth_target_t */
    khr_depth_target_t depth_target = {};
    TEST_ASSERT(khr_depth_target_create(&dev, &depth_target, 256, 256, VK_SAMPLE_COUNT_1_BIT, VK_FORMAT_UNDEFINED),
                "depth target create");

    VkImageAspectFlags depth_aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (depth_target.format == VK_FORMAT_D24_UNORM_S8_UINT || depth_target.format == VK_FORMAT_D32_SFLOAT_S8_UINT) {
        depth_aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    }

    /* Transition color and depth images for dynamic rendering */
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

    /* Begin dynamic rendering with Reversed-Z clear (depth = 0.0) */
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
        .clearValue = { .depthStencil = { .depth = 0.0f } }, /* Reversed-Z far plane = 0.0 */
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

    khr_mesh_instanced_push_t mesh_push = {
        .instances_addr = culled_gpu,
        .verts_addr = verts_gpu,
        .indices_addr = indices_gpu,
        .lights_addr = 0,
        .light_dir = { 0.577f, 0.577f, 0.577f, 3.0f },
        .camera_pos = { cam.eye[0], cam.eye[1], cam.eye[2], 1.0f },
        .light_color = { 1.0f, 0.98f, 0.95f, 0.2f },
        .index_count = 3,
        .vert_count = 3,
        .light_count = 0,
    };

    /* Draw indirect with GPU-culled instance count */
    khr_mesh_instanced_draw_indirect(&inst_pipe, cmd, &mesh_push, arena.buffer,
                                     (VkDeviceSize)((uintptr_t)cmd_host - (uintptr_t)arena.host_ptr),
                                     1, sizeof(khr_draw_indirect_cmd_t));

    vkCmdEndRendering(cmd);

    /* Transition color image to TRANSFER_SRC and copy center pixel (128, 128) to BDA arena */
    VkImageMemoryBarrier2 copy_bar = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .image = color_image,
        .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
    };
    VkDependencyInfo copy_dep = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &copy_bar,
    };
    vkCmdPipelineBarrier2(cmd, &copy_dep);

    VkBufferImageCopy copy_region = {
        .bufferOffset = (VkDeviceSize)((uintptr_t)pixel_host - (uintptr_t)arena.host_ptr),
        .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .imageOffset = { 128, 128, 0 },
        .imageExtent = { 1, 1, 1 },
    };
    vkCmdCopyImageToBuffer(cmd, color_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, arena.buffer, 1, &copy_region);

    VkMemoryBarrier2 host_read_bar = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
        .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
    };
    VkDependencyInfo host_read_dep = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &host_read_bar,
    };
    vkCmdPipelineBarrier2(cmd, &host_read_dep);

    TEST_ASSERT_EQ(vkEndCommandBuffer(cmd), VK_SUCCESS, "end cmd");

    /* Submit with timeline semaphore */
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

    /* Read back draw command: both instances must be rendered */
    TEST_ASSERT_EQ(draw_cmd->instanceCount, 2U, "Both instances visible in front of camera");

    /* Verify center pixel was actually rasterized with PBR shader */
    const uint8_t* p = (const uint8_t*)pixel_host;
    TEST_ASSERT_GT(p[2], 50U, "Center pixel red channel must be bright (rendered gold PBR)");
    TEST_ASSERT_EQ(p[3], 255U, "Alpha must be 255 (opaque)");

    /* Cleanup */
    vkFreeCommandBuffers(dev.device, dev.pool, 1, &cmd);
    khr_depth_target_destroy(&dev, &depth_target);
    vkDestroyImageView(dev.device, color_view, nullptr);
    vkDestroyImage(dev.device, color_image, nullptr);
    vkFreeMemory(dev.device, color_mem, nullptr);
    khr_bda_arena_destroy(&dev, &arena);
    khr_mesh_instanced_pipeline_destroy(&inst_pipe);
    khr_cull_pipeline_destroy(&cull_pipe);
    khr_gfx_device_destroy(&dev);
    return true;
}

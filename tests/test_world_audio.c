#include "test_framework.h"
#include "khoros/gfx/grid.h"
#include "khoros/gfx/scene.h"
#include "khoros/audio/synth.h"
#include "khoros/core/topology.h"
#include "khoros/core/physics.h"
#include "khoros/gfx/device.h"
#include "khoros/gfx/bda_arena.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

#define ASSERT_FLOAT_NEAR(a, b, eps, msg) do { \
    float _diff = fabsf((float)(a) - (float)(b)); \
    TEST_ASSERT(_diff <= (float)(eps), msg); \
} while (0)

[[nodiscard]]
bool test_grid_pipeline_layout_and_push_contract(void) {
    /* std430 alignment & size verification */
    TEST_ASSERT_EQ(sizeof(khr_grid_push_t), 64U, "khr_grid_push_t must be 64 bytes");
    TEST_ASSERT_EQ(alignof(khr_grid_push_t), 16U, "khr_grid_push_t must have 16-byte alignment");

    TEST_ASSERT_EQ(offsetof(khr_grid_push_t, eye_plane_y), 0U, "eye_plane_y at offset 0");
    TEST_ASSERT_EQ(offsetof(khr_grid_push_t, target_minor_sz), 16U, "target_minor_sz at offset 16");
    TEST_ASSERT_EQ(offsetof(khr_grid_push_t, up_major_sz), 32U, "up_major_sz at offset 32");
    TEST_ASSERT_EQ(offsetof(khr_grid_push_t, params), 48U, "params at offset 48");

    /* Vulkan 1.4 dynamic rendering pipeline creation */
    khr_gfx_device_t dev = {};
    TEST_ASSERT(khr_gfx_device_init(&dev, (dev_t)0), "init gfx device");

    VkFormat depth_fmt = khr_gfx_depth_format(&dev);
    khr_grid_pipeline_t grid = {};
    TEST_ASSERT(khr_grid_pipeline_init(&grid, &dev, VK_FORMAT_B8G8R8A8_UNORM, depth_fmt),
                "grid pipeline init");
    TEST_ASSERT(grid.layout != VK_NULL_HANDLE, "grid pipeline layout valid");
    TEST_ASSERT(grid.pipeline != VK_NULL_HANDLE, "grid pipeline handle valid");

    khr_grid_pipeline_destroy(&grid);
    TEST_ASSERT(grid.pipeline == VK_NULL_HANDLE, "pipeline cleared on destroy");
    TEST_ASSERT(grid.layout == VK_NULL_HANDLE, "layout cleared on destroy");

    khr_gfx_device_destroy(&dev);
    return true;
}

[[nodiscard]]
bool test_procedural_sphere_generation(void) {
    khr_gfx_device_t dev = {};
    TEST_ASSERT(khr_gfx_device_init(&dev, (dev_t)0), "init gfx device");

    khr_bda_arena_t arena = {};
    TEST_ASSERT(khr_bda_arena_init(&dev, &arena, 256 * 1024), "init arena");

    VkDeviceAddress verts_addr = 0, normals_addr = 0, indices_addr = 0;
    uint32_t vert_count = 0, index_count = 0;
    constexpr float R = 2.0f;
    constexpr uint32_t rings = 12;
    constexpr uint32_t sectors = 24;

    TEST_ASSERT(khr_scene_generate_sphere(&arena, R, rings, sectors,
                                          &verts_addr, &normals_addr, &indices_addr,
                                          &vert_count, &index_count),
                "generate sphere");

    uint32_t expected_verts = (rings + 1) * (sectors + 1);
    uint32_t expected_indices = rings * sectors * 6;
    TEST_ASSERT_EQ(vert_count, expected_verts, "sphere vertex count matches formula");
    TEST_ASSERT_EQ(index_count, expected_indices, "sphere index count matches formula");

    /* Map arena host pointer to inspect geometric invariants */
    const float* verts = (const float*)((uint8_t*)arena.host_ptr + (verts_addr - arena.gpu_address));
    const float* normals = (const float*)((uint8_t*)arena.host_ptr + (normals_addr - arena.gpu_address));
    const uint32_t* indices = (const uint32_t*)((uint8_t*)arena.host_ptr + (indices_addr - arena.gpu_address));

    /* Check north pole */
    ASSERT_FLOAT_NEAR(verts[0], 0.0f, 1e-4f, "pole x");
    ASSERT_FLOAT_NEAR(verts[1], R, 1e-4f, "pole y");
    ASSERT_FLOAT_NEAR(verts[2], 0.0f, 1e-4f, "pole z");

    /* Check every vertex is on sphere surface and normals are unit length */
    for (uint32_t v = 0; v < vert_count; v++) {
        float x = verts[v * 3 + 0];
        float y = verts[v * 3 + 1];
        float z = verts[v * 3 + 2];
        float dist = sqrtf(x * x + y * y + z * z);
        ASSERT_FLOAT_NEAR(dist, R, 1e-3f, "vertex distance from origin equals radius");

        float nx = normals[v * 3 + 0];
        float ny = normals[v * 3 + 1];
        float nz = normals[v * 3 + 2];
        float nlen = sqrtf(nx * nx + ny * ny + nz * nz);
        ASSERT_FLOAT_NEAR(nlen, 1.0f, 1e-3f, "normal is unit vector");

        float dot = (x * nx + y * ny + z * nz) / dist;
        ASSERT_FLOAT_NEAR(dot, 1.0f, 1e-3f, "normal points outward along radius");
    }

    /* Verify all indices are in valid range */
    for (uint32_t i = 0; i < index_count; i++) {
        TEST_ASSERT(indices[i] < vert_count, "index within vertex bounds");
    }

    khr_bda_arena_destroy(&dev, &arena);
    khr_gfx_device_destroy(&dev);
    return true;
}

[[nodiscard]]
bool test_procedural_cylinder_generation(void) {
    khr_gfx_device_t dev = {};
    TEST_ASSERT(khr_gfx_device_init(&dev, (dev_t)0), "init gfx device");

    khr_bda_arena_t arena = {};
    TEST_ASSERT(khr_bda_arena_init(&dev, &arena, 256 * 1024), "init arena");

    VkDeviceAddress verts_addr = 0, normals_addr = 0, indices_addr = 0;
    uint32_t vert_count = 0, index_count = 0;
    constexpr float R = 1.5f;
    constexpr float H = 4.0f;
    constexpr uint32_t S = 16;

    TEST_ASSERT(khr_scene_generate_cylinder(&arena, R, H, S,
                                            &verts_addr, &normals_addr, &indices_addr,
                                            &vert_count, &index_count),
                "generate cylinder");

    uint32_t expected_verts = (S + 1) * 2 + 1 + (S + 1) + 1 + (S + 1);
    uint32_t expected_indices = S * 6 + S * 3 + S * 3;
    TEST_ASSERT_EQ(vert_count, expected_verts, "cylinder vertex count matches formula");
    TEST_ASSERT_EQ(index_count, expected_indices, "cylinder index count matches formula");

    const float* verts = (const float*)((uint8_t*)arena.host_ptr + (verts_addr - arena.gpu_address));
    const float* normals = (const float*)((uint8_t*)arena.host_ptr + (normals_addr - arena.gpu_address));
    const uint32_t* indices = (const uint32_t*)((uint8_t*)arena.host_ptr + (indices_addr - arena.gpu_address));

    /* Check heights of all vertices lie within [-H/2, +H/2] */
    float half_h = H * 0.5f;
    for (uint32_t v = 0; v < vert_count; v++) {
        float y = verts[v * 3 + 1];
        TEST_ASSERT(y >= -half_h - 1e-4f && y <= half_h + 1e-4f, "vertex y in range");

        float nx = normals[v * 3 + 0];
        float ny = normals[v * 3 + 1];
        float nz = normals[v * 3 + 2];
        float nlen = sqrtf(nx * nx + ny * ny + nz * nz);
        ASSERT_FLOAT_NEAR(nlen, 1.0f, 1e-3f, "normal length is 1");
    }

    for (uint32_t i = 0; i < index_count; i++) {
        TEST_ASSERT(indices[i] < vert_count, "index in bounds");
    }

    khr_bda_arena_destroy(&dev, &arena);
    khr_gfx_device_destroy(&dev);
    return true;
}

[[nodiscard]]
bool test_audio_synth_impact_modal_resonance(void) {
    constexpr uint32_t cap = 24000;
    float* samples = calloc(cap, sizeof(float));
    TEST_ASSERT_NOT_NULL(samples, "alloc samples");

    khr_audio_clip_t clip = {};
    TEST_ASSERT(khr_audio_synth_impact(&clip, samples, cap, 48000, 520.0f, 0.45f), "synth impact");
    TEST_ASSERT_EQ(clip.sample_rate, 48000U, "sample rate 48kHz");
    TEST_ASSERT(clip.frame_count > 0 && clip.frame_count <= cap, "valid frame count");

    /* Peak normalization check */
    float peak = 0.0f;
    for (uint32_t i = 0; i < clip.frame_count; i++) {
        float a = fabsf(samples[i]);
        if (a > peak) peak = a;
    }
    TEST_ASSERT(peak > 0.2f && peak <= 0.95f, "peak within normalized bounds");

    /* Energy decay check: Head energy must be far greater than tail energy */
    float head_energy = 0.0f;
    float tail_energy = 0.0f;
    for (uint32_t i = 0; i < 1000; i++) {
        head_energy += samples[i] * samples[i];
    }
    for (uint32_t i = clip.frame_count - 1000; i < clip.frame_count; i++) {
        tail_energy += samples[i] * samples[i];
    }
    TEST_ASSERT(head_energy > tail_energy * 10.0f, "modal exponential damping confirmed");

    free(samples);
    return true;
}

[[nodiscard]]
bool test_audio_synth_thud_decay(void) {
    constexpr uint32_t cap = 14400;
    float* samples = calloc(cap, sizeof(float));
    TEST_ASSERT_NOT_NULL(samples, "alloc samples");

    khr_audio_clip_t clip = {};
    TEST_ASSERT(khr_audio_synth_thud(&clip, samples, cap, 48000, 85.0f, 0.30f), "synth thud");
    TEST_ASSERT_EQ(clip.sample_rate, 48000U, "sample rate 48kHz");
    TEST_ASSERT(clip.frame_count > 0, "frames generated");

    /* Rapid dissipation check: amplitude at 0.25s must be tiny */
    float peak = 0.0f;
    for (uint32_t i = 0; i < clip.frame_count; i++) {
        float a = fabsf(samples[i]);
        if (a > peak) peak = a;
    }
    TEST_ASSERT(peak > 0.2f, "thud has impact energy");

    uint32_t idx_late = (uint32_t)(0.25f * 48000.0f);
    if (idx_late < clip.frame_count) {
        float late_amp = fabsf(samples[idx_late]);
        TEST_ASSERT(late_amp < peak * 0.08f, "thud decays within 250ms");
    }

    free(samples);
    return true;
}

[[nodiscard]]
bool test_audio_synth_click_transient(void) {
    constexpr uint32_t cap = 2400;
    float* samples = calloc(cap, sizeof(float));
    TEST_ASSERT_NOT_NULL(samples, "alloc samples");

    khr_audio_clip_t clip = {};
    TEST_ASSERT(khr_audio_synth_click(&clip, samples, cap, 48000, 1800.0f, 0.05f), "synth click");
    TEST_ASSERT_EQ(clip.sample_rate, 48000U, "sample rate 48kHz");
    TEST_ASSERT(clip.frame_count <= cap, "frames in limit");

    /* Click must peak in the first 2ms */
    float max_val = 0.0f;
    uint32_t max_idx = 0;
    for (uint32_t i = 0; i < clip.frame_count; i++) {
        float a = fabsf(samples[i]);
        if (a > max_val) {
            max_val = a;
            max_idx = i;
        }
    }
    TEST_ASSERT(max_idx < 96, "transient peak occurs within first 2ms");

    free(samples);
    return true;
}

[[nodiscard]]
bool test_physics_collision_event_queue(void) {
    khr_topology_t topo = {};

    khr_gpu_instance_t buf_a[2] = {};
    khr_gpu_instance_t buf_b[2] = {};

    /* Sphere at Y = -2.0, radius 0.5 (bottom at Y = -2.5) */
    buf_a[0].position[0] = 0.0f;
    buf_a[0].position[1] = -2.0f;
    buf_a[0].position[2] = 0.0f;
    buf_a[0].radius = 0.5f;

    TEST_ASSERT(khr_topology_start_sim(&topo, 120, 1, buf_a, buf_b, 0x1000, 0x2000), "start sim");
    khr_topology_sim_set_motion(&topo, true);

    /* Apply downward impulse to smash sphere into floor at Y = -2.5 */
    float impulse[3] = { 0.0f, -15.0f, 0.0f };
    float rel_pos[3] = { 0.0f, 0.0f, 0.0f };
    khr_topology_sim_apply_impulse(&topo, 0, impulse, rel_pos);

    /* Step simulation to trigger collision */
    bool saw_event = false;
    for (uint32_t step = 0; step < 20; step++) {
        khr_topology_sim_step(&topo, step + 1);

        khr_collision_event_t evt = {};
        while (khr_topology_sim_pop_collision_event(&topo, &evt)) {
            saw_event = true;
            TEST_ASSERT(evt.impulse > 0.0f, "collision has non-zero impulse");
            TEST_ASSERT_EQ(evt.sound_type, (uint32_t)KHR_COLLISION_SOUND_THUD, "floor collision produces THUD");
            TEST_ASSERT(evt.normal[1] > 0.8f, "floor normal points upwards");
        }
    }
    TEST_ASSERT(saw_event, "collision event successfully harvested from FIFO queue");

    /* Empty queue test */
    khr_collision_event_t empty_evt = {};
    TEST_ASSERT(!khr_topology_sim_pop_collision_event(&topo, &empty_evt), "empty queue returns false");

    khr_topology_stop_sim(&topo);
    return true;
}

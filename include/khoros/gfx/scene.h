#ifndef KHOROS_GFX_SCENE_H
#define KHOROS_GFX_SCENE_H

#if __STDC_VERSION__ < 202311L
#error "Khoros requires pure ISO C23 (compile with -std=c23)"
#endif

#ifdef _STDBOOL_H
#error "<stdbool.h> is strictly banned. In C23, bool, true, and false are native language keywords."
#endif

#ifdef _STDALIGN_H
#error "<stdalign.h> is strictly banned. In C23, alignas and alignof are native language keywords."
#endif

#include "khoros/core/attributes.h"
#include "khoros/gfx/device.h"
#include "khoros/gfx/bda_arena.h"
#include "khoros/gfx/gpu_math.h"
#include "khoros/gfx/camera.h"
#include "khoros/gfx/hiz.h"
#include <stdint.h>
#include <vulkan/vulkan.h>

/* Maximum meshes and capacities in a scene */
constexpr uint32_t KHR_SCENE_MAX_MESHES = 16;
constexpr uint32_t KHR_SCENE_DEFAULT_MAX_INSTANCES = 256;
constexpr uint32_t KHR_SCENE_DEFAULT_MAX_LIGHTS = 16;

/*
 * Registered Mesh Asset Descriptor inside BDA memory.
 */
typedef struct {
    uint32_t        mesh_id;
    VkDeviceAddress verts_addr;
    VkDeviceAddress indices_addr;
    VkDeviceAddress normals_addr;
    uint32_t        vert_count;
    uint32_t        index_count;
    float           bounding_radius;
} khr_scene_mesh_t;

/*
 * Multi-Mesh Scene Graph & Instance Manager (Pillar D).
 * Coordinates multiple meshes, per-instance TRS & PBR materials, and physical lights.
 */
typedef struct {
    const khr_gfx_device_t* dev;
    khr_bda_arena_t*        arena;

    /* Mesh registry */
    khr_scene_mesh_t meshes[KHR_SCENE_MAX_MESHES];
    uint32_t         mesh_count;

    /* Instance registry (GPU BDA backing with double buffering for sim) */
    khr_gpu_instance_t*        instances;
    VkDeviceAddress            instances_gpu;
    khr_gpu_instance_t*        instances_b;
    VkDeviceAddress            instances_b_gpu;
    khr_gpu_culled_instance_t* culled_instances;
    VkDeviceAddress            culled_instances_gpu;
    uint32_t                   instance_count;
    uint32_t                   max_instances;

    /* Draw indirect command buffer (GPU BDA backing) */
    khr_draw_indirect_cmd_t* draw_cmd;
    VkDeviceAddress          draw_cmd_gpu;

    /* Atomic count buffer (GPU BDA backing) */
    uint32_t*       draw_count;
    VkDeviceAddress draw_count_gpu;

    /* Physical light registry (GPU BDA backing) */
    khr_gpu_light_t* lights;
    VkDeviceAddress  lights_gpu;
    uint32_t         light_count;
    uint32_t         max_lights;
} khr_scene_t;

/*
 * Initialize multi-mesh scene manager and allocate BDA backing buffers.
 */
[[nodiscard]]
bool khr_scene_init(khr_scene_t* scene,
                    const khr_gfx_device_t* dev,
                    khr_bda_arena_t* arena,
                    uint32_t max_instances,
                    uint32_t max_lights);

/*
 * Destroy scene and release tracking state (arena memory freed on arena destroy).
 */
void khr_scene_destroy(khr_scene_t* scene);

/*
 * Register a mesh asset into the scene.
 */
[[nodiscard]]
bool khr_scene_register_mesh(khr_scene_t* scene,
                             uint32_t mesh_id,
                             VkDeviceAddress verts_addr,
                             VkDeviceAddress indices_addr,
                             uint32_t vert_count,
                             uint32_t index_count,
                             float bounding_radius);

/*
 * Register a mesh asset with explicit normals into the scene.
 */
[[nodiscard]]
bool khr_scene_register_mesh_normals(khr_scene_t* scene,
                                     uint32_t mesh_id,
                                     VkDeviceAddress verts_addr,
                                     VkDeviceAddress indices_addr,
                                     VkDeviceAddress normals_addr,
                                     uint32_t vert_count,
                                     uint32_t index_count,
                                     float bounding_radius);

/*
 * Add a new instance to the scene with TRS transform and Cook-Torrance PBR material.
 * Returns instance index, or UINT32_MAX on capacity overflow.
 */
uint32_t khr_scene_add_instance(khr_scene_t* scene, const khr_gpu_instance_t* inst);

/*
 * Update TRS transform of an existing instance.
 */
void khr_scene_update_instance_trs(khr_scene_t* scene,
                                   uint32_t instance_id,
                                   const float pos[3],
                                   const float rot[4],
                                   const float scale[3]);

/*
 * Add a physical light source (Directional, Point, or Spot) to the scene.
 * Returns light index, or UINT32_MAX on capacity overflow.
 */
uint32_t khr_scene_add_light(khr_scene_t* scene, const khr_gpu_light_t* light);

/*
 * Update parameters of an existing physical light source.
 */
void khr_scene_update_light(khr_scene_t* scene,
                            uint32_t light_id,
                            const khr_gpu_light_t* light);

/*
 * Procedural mesh generators allocated directly inside BDA arena.
 */
[[nodiscard]]
bool khr_scene_generate_cube(khr_bda_arena_t* arena,
                             VkDeviceAddress* out_verts,
                             VkDeviceAddress* out_indices,
                             uint32_t* out_vert_count,
                             uint32_t* out_index_count);

[[nodiscard]]
bool khr_scene_generate_plane(khr_bda_arena_t* arena,
                              float half_size,
                              VkDeviceAddress* out_verts,
                              VkDeviceAddress* out_indices,
                              uint32_t* out_vert_count,
                              uint32_t* out_index_count);

[[nodiscard]]
bool khr_scene_generate_sphere(khr_bda_arena_t* arena,
                               float radius,
                               uint32_t rings,
                               uint32_t sectors,
                               VkDeviceAddress* out_verts,
                               VkDeviceAddress* out_normals,
                               VkDeviceAddress* out_indices,
                               uint32_t* out_vert_count,
                               uint32_t* out_index_count);

[[nodiscard]]
bool khr_scene_generate_cylinder(khr_bda_arena_t* arena,
                                 float radius,
                                 float height,
                                 uint32_t segments,
                                 VkDeviceAddress* out_verts,
                                 VkDeviceAddress* out_normals,
                                 VkDeviceAddress* out_indices,
                                 uint32_t* out_vert_count,
                                 uint32_t* out_index_count);

[[nodiscard]]
bool khr_scene_generate_chamfer_box(khr_bda_arena_t* arena,
                                    float half_size,
                                    float chamfer_size,
                                    VkDeviceAddress* out_verts,
                                    VkDeviceAddress* out_normals,
                                    VkDeviceAddress* out_indices,
                                    uint32_t* out_vert_count,
                                    uint32_t* out_index_count);

[[nodiscard]]
bool khr_scene_generate_torus(khr_bda_arena_t* arena,
                              float major_radius,
                              float minor_radius,
                              uint32_t major_segments,
                              uint32_t minor_segments,
                              VkDeviceAddress* out_verts,
                              VkDeviceAddress* out_normals,
                              VkDeviceAddress* out_indices,
                              uint32_t* out_vert_count,
                              uint32_t* out_index_count);

/*
 * Prepare push constants for compute culling and instanced drawing.
 */
void khr_scene_prepare_cull_push(const khr_scene_t* scene,
                                 const khr_camera_t* cam,
                                 khr_cull_push_t* out_push);

void khr_scene_prepare_hiz_cull_push(const khr_scene_t* scene,
                                     const khr_camera_t* cam,
                                     const khr_hiz_t* hiz,
                                     khr_hiz_cull_push_t* out_push);

void khr_scene_prepare_mesh_push(const khr_scene_t* scene,
                                 uint32_t mesh_id,
                                 const khr_camera_t* cam,
                                 khr_mesh_instanced_push_t* out_push);

#endif /* KHOROS_GFX_SCENE_H */

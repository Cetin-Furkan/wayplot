#include "khoros/gfx/scene.h"
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static inline void khr_scene_add_quad(uint32_t* i_ptr, uint32_t* ii,
                                      uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    i_ptr[(*ii)++] = a; i_ptr[(*ii)++] = b; i_ptr[(*ii)++] = c;
    i_ptr[(*ii)++] = a; i_ptr[(*ii)++] = c; i_ptr[(*ii)++] = d;
}

[[nodiscard]]
bool khr_scene_init(khr_scene_t* scene,
                    const khr_gfx_device_t* dev,
                    khr_bda_arena_t* arena,
                    uint32_t max_instances,
                    uint32_t max_lights) {
    if (scene == nullptr || dev == nullptr || arena == nullptr) {
        return false;
    }
    *scene = (khr_scene_t){
        .dev = dev,
        .arena = arena,
        .max_instances = max_instances > 0 ? max_instances : KHR_SCENE_DEFAULT_MAX_INSTANCES,
        .max_lights = max_lights > 0 ? max_lights : KHR_SCENE_DEFAULT_MAX_LIGHTS,
    };

    /* Allocate GPU BDA buffer for input instances with double buffering (Slot A & Slot B) */
    void* inst_host = nullptr;
    if (!khr_bda_arena_alloc(arena, 2 * scene->max_instances * sizeof(khr_gpu_instance_t), 64,
                             &inst_host, &scene->instances_gpu)) {
        return false;
    }
    scene->instances = (khr_gpu_instance_t*)inst_host;
    scene->instances_b = scene->instances + scene->max_instances;
    scene->instances_b_gpu = scene->instances_gpu + (VkDeviceAddress)(scene->max_instances * sizeof(khr_gpu_instance_t));

    /* Allocate GPU BDA buffer for culled instances */
    void* culled_host = nullptr;
    if (!khr_bda_arena_alloc(arena, scene->max_instances * sizeof(khr_gpu_culled_instance_t), 64,
                             &culled_host, &scene->culled_instances_gpu)) {
        return false;
    }
    scene->culled_instances = (khr_gpu_culled_instance_t*)culled_host;

    /* Allocate GPU BDA buffer for draw indirect commands (up to KHR_SCENE_MAX_MESHES) */
    void* cmd_host = nullptr;
    if (!khr_bda_arena_alloc(arena, KHR_SCENE_MAX_MESHES * sizeof(khr_draw_indirect_cmd_t), 64,
                             &cmd_host, &scene->draw_cmd_gpu)) {
        return false;
    }
    scene->draw_cmd = (khr_draw_indirect_cmd_t*)cmd_host;

    /* Allocate GPU BDA buffer for atomic draw counts (up to KHR_SCENE_MAX_MESHES) */
    void* count_host = nullptr;
    if (!khr_bda_arena_alloc(arena, KHR_SCENE_MAX_MESHES * sizeof(uint32_t), 64,
                             &count_host, &scene->draw_count_gpu)) {
        return false;
    }
    scene->draw_count = (uint32_t*)count_host;

    /* Allocate GPU BDA buffer for physical lights */
    void* lights_host = nullptr;
    if (!khr_bda_arena_alloc(arena, scene->max_lights * sizeof(khr_gpu_light_t), 64,
                             &lights_host, &scene->lights_gpu)) {
        return false;
    }
    scene->lights = (khr_gpu_light_t*)lights_host;

    return true;
}

void khr_scene_destroy(khr_scene_t* scene) {
    if (scene == nullptr) {
        return;
    }
    *scene = (khr_scene_t){};
}

[[nodiscard]]
bool khr_scene_register_mesh_normals(khr_scene_t* scene,
                                     uint32_t mesh_id,
                                     VkDeviceAddress verts_addr,
                                     VkDeviceAddress indices_addr,
                                     VkDeviceAddress normals_addr,
                                     uint32_t vert_count,
                                     uint32_t index_count,
                                     float bounding_radius) {
    if (scene == nullptr || verts_addr == 0 || indices_addr == 0 || index_count == 0) {
        return false;
    }
    for (uint32_t i = 0; i < scene->mesh_count; i++) {
        if (scene->meshes[i].mesh_id == mesh_id) {
            scene->meshes[i] = (khr_scene_mesh_t){
                .mesh_id = mesh_id,
                .verts_addr = verts_addr,
                .indices_addr = indices_addr,
                .normals_addr = normals_addr,
                .vert_count = vert_count,
                .index_count = index_count,
                .bounding_radius = bounding_radius > 0.0f ? bounding_radius : 1.0f,
            };
            return true;
        }
    }
    if (scene->mesh_count >= KHR_SCENE_MAX_MESHES) {
        return false;
    }
    scene->meshes[scene->mesh_count++] = (khr_scene_mesh_t){
        .mesh_id = mesh_id,
        .verts_addr = verts_addr,
        .indices_addr = indices_addr,
        .normals_addr = normals_addr,
        .vert_count = vert_count,
        .index_count = index_count,
        .bounding_radius = bounding_radius > 0.0f ? bounding_radius : 1.0f,
    };
    return true;
}

[[nodiscard]]
bool khr_scene_register_mesh(khr_scene_t* scene,
                             uint32_t mesh_id,
                             VkDeviceAddress verts_addr,
                             VkDeviceAddress indices_addr,
                             uint32_t vert_count,
                             uint32_t index_count,
                             float bounding_radius) {
    return khr_scene_register_mesh_normals(scene, mesh_id, verts_addr, indices_addr,
                                           0, vert_count, index_count, bounding_radius);
}

uint32_t khr_scene_add_instance(khr_scene_t* scene, const khr_gpu_instance_t* inst) {
    if (scene == nullptr || inst == nullptr || scene->instance_count >= scene->max_instances) {
        return UINT32_MAX;
    }
    uint32_t id = scene->instance_count++;
    scene->instances[id] = *inst;
    if (scene->instances_b != nullptr) {
        scene->instances_b[id] = *inst;
    }
    return id;
}

void khr_scene_update_instance_trs(khr_scene_t* scene,
                                   uint32_t instance_id,
                                   const float pos[3],
                                   const float rot[4],
                                   const float scale[3]) {
    if (scene == nullptr || instance_id >= scene->instance_count) {
        return;
    }
    khr_gpu_instance_t* inst = &scene->instances[instance_id];
    if (pos != nullptr) {
        inst->position[0] = pos[0];
        inst->position[1] = pos[1];
        inst->position[2] = pos[2];
    }
    if (rot != nullptr) {
        inst->rotation[0] = rot[0];
        inst->rotation[1] = rot[1];
        inst->rotation[2] = rot[2];
        inst->rotation[3] = rot[3];
    }
    if (scale != nullptr) {
        inst->scale[0] = scale[0];
        inst->scale[1] = scale[1];
        inst->scale[2] = scale[2];
    }
    if (scene->instances_b != nullptr) {
        scene->instances_b[instance_id] = *inst;
    }
}

uint32_t khr_scene_add_light(khr_scene_t* scene, const khr_gpu_light_t* light) {
    if (scene == nullptr || light == nullptr || scene->light_count >= scene->max_lights) {
        return UINT32_MAX;
    }
    uint32_t id = scene->light_count++;
    scene->lights[id] = *light;
    return id;
}

void khr_scene_update_light(khr_scene_t* scene,
                            uint32_t light_id,
                            const khr_gpu_light_t* light) {
    if (scene == nullptr || light_id >= scene->light_count || light == nullptr) {
        return;
    }
    scene->lights[light_id] = *light;
}

[[nodiscard]]
bool khr_scene_generate_cube(khr_bda_arena_t* arena,
                             VkDeviceAddress* out_verts,
                             VkDeviceAddress* out_indices,
                             uint32_t* out_vert_count,
                             uint32_t* out_index_count) {
    if (arena == nullptr || out_verts == nullptr || out_indices == nullptr ||
        out_vert_count == nullptr || out_index_count == nullptr) {
        return false;
    }

    /* 8 vertices: (+-0.5, +-0.5, +-0.5) */
    const float verts[24] = {
        -0.5f, -0.5f, -0.5f,
         0.5f, -0.5f, -0.5f,
         0.5f,  0.5f, -0.5f,
        -0.5f,  0.5f, -0.5f,
        -0.5f, -0.5f,  0.5f,
         0.5f, -0.5f,  0.5f,
         0.5f,  0.5f,  0.5f,
        -0.5f,  0.5f,  0.5f,
    };

    /* 36 indices: 6 faces * 2 triangles * 3 indices (CCW order) */
    const uint32_t indices[36] = {
        // Front (Z+)
        4, 5, 6,  4, 6, 7,
        // Back (Z-)
        1, 0, 3,  1, 3, 2,
        // Top (Y+)
        3, 2, 6,  3, 6, 7,
        // Bottom (Y-)
        0, 1, 5,  0, 5, 4,
        // Right (X+)
        5, 1, 2,  5, 2, 6,
        // Left (X-)
        0, 4, 7,  0, 7, 3,
    };

    void* vhost = nullptr;
    if (!khr_bda_arena_alloc(arena, sizeof(verts), 16, &vhost, out_verts)) {
        return false;
    }
    memcpy(vhost, verts, sizeof(verts));

    void* ihost = nullptr;
    if (!khr_bda_arena_alloc(arena, sizeof(indices), 16, &ihost, out_indices)) {
        return false;
    }
    memcpy(ihost, indices, sizeof(indices));

    *out_vert_count = 8;
    *out_index_count = 36;
    return true;
}

[[nodiscard]]
bool khr_scene_generate_plane(khr_bda_arena_t* arena,
                              float half_size,
                              VkDeviceAddress* out_verts,
                              VkDeviceAddress* out_indices,
                              uint32_t* out_vert_count,
                              uint32_t* out_index_count) {
    if (arena == nullptr || out_verts == nullptr || out_indices == nullptr ||
        out_vert_count == nullptr || out_index_count == nullptr) {
        return false;
    }
    float s = half_size > 0.01f ? half_size : 1.0f;

    /* 4 vertices in XZ plane (Y = 0) */
    const float verts[12] = {
        -s, 0.0f, -s,
         s, 0.0f, -s,
         s, 0.0f,  s,
        -s, 0.0f,  s,
    };

    /* 6 indices: 2 triangles (Y-up normal) */
    const uint32_t indices[6] = {
        0, 2, 1,
        0, 3, 2,
    };

    void* vhost = nullptr;
    if (!khr_bda_arena_alloc(arena, sizeof(verts), 16, &vhost, out_verts)) {
        return false;
    }
    memcpy(vhost, verts, sizeof(verts));

    void* ihost = nullptr;
    if (!khr_bda_arena_alloc(arena, sizeof(indices), 16, &ihost, out_indices)) {
        return false;
    }
    memcpy(ihost, indices, sizeof(indices));

    *out_vert_count = 4;
    *out_index_count = 6;
    return true;
}

[[nodiscard]]
bool khr_scene_generate_sphere(khr_bda_arena_t* arena,
                               float radius,
                               uint32_t rings,
                               uint32_t sectors,
                               VkDeviceAddress* out_verts,
                               VkDeviceAddress* out_normals,
                               VkDeviceAddress* out_indices,
                               uint32_t* out_vert_count,
                               uint32_t* out_index_count) {
    if (arena == nullptr || out_verts == nullptr || out_indices == nullptr ||
        out_vert_count == nullptr || out_index_count == nullptr) {
        return false;
    }
    float r = (radius > 0.01f) ? radius : 1.0f;
    uint32_t R = (rings >= 4) ? rings : 16;
    uint32_t S = (sectors >= 4) ? sectors : 24;

    uint32_t v_count = (R + 1) * (S + 1);
    uint32_t i_count = R * S * 6;

    void* vhost = nullptr;
    void* nhost = nullptr;
    void* ihost = nullptr;
    VkDeviceAddress ngpu = 0;

    if (!khr_bda_arena_alloc(arena, (size_t)v_count * 3 * sizeof(float), 16, &vhost, out_verts)) {
        return false;
    }
    if (out_normals != nullptr) {
        if (!khr_bda_arena_alloc(arena, (size_t)v_count * 3 * sizeof(float), 16, &nhost, &ngpu)) {
            return false;
        }
        *out_normals = ngpu;
    }
    if (!khr_bda_arena_alloc(arena, (size_t)i_count * sizeof(uint32_t), 16, &ihost, out_indices)) {
        return false;
    }

    float* v_ptr = (float*)vhost;
    float* n_ptr = (float*)nhost;
    uint32_t* i_ptr = (uint32_t*)ihost;

    uint32_t v_idx = 0;
    for (uint32_t ring = 0; ring <= R; ring++) {
        float theta = (float)ring * (float)M_PI / (float)R;
        float y = cosf(theta);
        float sin_t = sinf(theta);

        for (uint32_t sec = 0; sec <= S; sec++) {
            float phi = (float)sec * 2.0f * (float)M_PI / (float)S;
            float x = sin_t * cosf(phi);
            float z = sin_t * sinf(phi);

            v_ptr[v_idx * 3 + 0] = x * r;
            v_ptr[v_idx * 3 + 1] = y * r;
            v_ptr[v_idx * 3 + 2] = z * r;

            if (n_ptr != nullptr) {
                n_ptr[v_idx * 3 + 0] = x;
                n_ptr[v_idx * 3 + 1] = y;
                n_ptr[v_idx * 3 + 2] = z;
            }
            v_idx++;
        }
    }

    uint32_t i_idx = 0;
    for (uint32_t ring = 0; ring < R; ring++) {
        for (uint32_t sec = 0; sec < S; sec++) {
            uint32_t p0 = ring * (S + 1) + sec;
            uint32_t p1 = p0 + 1;
            uint32_t p2 = (ring + 1) * (S + 1) + sec;
            uint32_t p3 = p2 + 1;

            i_ptr[i_idx++] = p0;
            i_ptr[i_idx++] = p2;
            i_ptr[i_idx++] = p1;

            i_ptr[i_idx++] = p1;
            i_ptr[i_idx++] = p2;
            i_ptr[i_idx++] = p3;
        }
    }

    *out_vert_count = v_count;
    *out_index_count = i_count;
    return true;
}

[[nodiscard]]
bool khr_scene_generate_cylinder(khr_bda_arena_t* arena,
                                 float radius,
                                 float height,
                                 uint32_t segments,
                                 VkDeviceAddress* out_verts,
                                 VkDeviceAddress* out_normals,
                                 VkDeviceAddress* out_indices,
                                 uint32_t* out_vert_count,
                                 uint32_t* out_index_count) {
    if (arena == nullptr || out_verts == nullptr || out_indices == nullptr ||
        out_vert_count == nullptr || out_index_count == nullptr) {
        return false;
    }
    float r = (radius > 0.01f) ? radius : 1.0f;
    float h = (height > 0.01f) ? height : 2.0f;
    uint32_t S = (segments >= 6) ? segments : 24;

    uint32_t v_count = 4 * (S + 1) + 2;
    uint32_t i_count = 12 * S;

    void* vhost = nullptr;
    void* nhost = nullptr;
    void* ihost = nullptr;
    VkDeviceAddress ngpu = 0;

    if (!khr_bda_arena_alloc(arena, (size_t)v_count * 3 * sizeof(float), 16, &vhost, out_verts)) {
        return false;
    }
    if (out_normals != nullptr) {
        if (!khr_bda_arena_alloc(arena, (size_t)v_count * 3 * sizeof(float), 16, &nhost, &ngpu)) {
            return false;
        }
        *out_normals = ngpu;
    }
    if (!khr_bda_arena_alloc(arena, (size_t)i_count * sizeof(uint32_t), 16, &ihost, out_indices)) {
        return false;
    }

    float* v_ptr = (float*)vhost;
    float* n_ptr = (float*)nhost;
    uint32_t* i_ptr = (uint32_t*)ihost;

    float half_h = h * 0.5f;
    uint32_t v_idx = 0;

    /* 1. Body Vertices */
    uint32_t body_start = v_idx;
    for (uint32_t ring = 0; ring < 2; ring++) {
        float y = (ring == 0) ? -half_h : half_h;
        for (uint32_t sec = 0; sec <= S; sec++) {
            float phi = (float)sec * 2.0f * (float)M_PI / (float)S;
            float cos_p = cosf(phi);
            float sin_p = sinf(phi);

            v_ptr[v_idx * 3 + 0] = r * cos_p;
            v_ptr[v_idx * 3 + 1] = y;
            v_ptr[v_idx * 3 + 2] = r * sin_p;

            if (n_ptr) {
                n_ptr[v_idx * 3 + 0] = cos_p;
                n_ptr[v_idx * 3 + 1] = 0.0f;
                n_ptr[v_idx * 3 + 2] = sin_p;
            }
            v_idx++;
        }
    }

    /* 2. Top Cap */
    uint32_t top_center = v_idx;
    v_ptr[v_idx * 3 + 0] = 0.0f;
    v_ptr[v_idx * 3 + 1] = half_h;
    v_ptr[v_idx * 3 + 2] = 0.0f;
    if (n_ptr) { n_ptr[v_idx * 3 + 0] = 0.0f; n_ptr[v_idx * 3 + 1] = 1.0f; n_ptr[v_idx * 3 + 2] = 0.0f; }
    v_idx++;

    uint32_t top_rim = v_idx;
    for (uint32_t sec = 0; sec <= S; sec++) {
        float phi = (float)sec * 2.0f * (float)M_PI / (float)S;
        v_ptr[v_idx * 3 + 0] = r * cosf(phi);
        v_ptr[v_idx * 3 + 1] = half_h;
        v_ptr[v_idx * 3 + 2] = r * sinf(phi);
        if (n_ptr) { n_ptr[v_idx * 3 + 0] = 0.0f; n_ptr[v_idx * 3 + 1] = 1.0f; n_ptr[v_idx * 3 + 2] = 0.0f; }
        v_idx++;
    }

    /* 3. Bottom Cap */
    uint32_t bot_center = v_idx;
    v_ptr[v_idx * 3 + 0] = 0.0f;
    v_ptr[v_idx * 3 + 1] = -half_h;
    v_ptr[v_idx * 3 + 2] = 0.0f;
    if (n_ptr) { n_ptr[v_idx * 3 + 0] = 0.0f; n_ptr[v_idx * 3 + 1] = -1.0f; n_ptr[v_idx * 3 + 2] = 0.0f; }
    v_idx++;

    uint32_t bot_rim = v_idx;
    for (uint32_t sec = 0; sec <= S; sec++) {
        float phi = (float)sec * 2.0f * (float)M_PI / (float)S;
        v_ptr[v_idx * 3 + 0] = r * cosf(phi);
        v_ptr[v_idx * 3 + 1] = -half_h;
        v_ptr[v_idx * 3 + 2] = r * sinf(phi);
        if (n_ptr) { n_ptr[v_idx * 3 + 0] = 0.0f; n_ptr[v_idx * 3 + 1] = -1.0f; n_ptr[v_idx * 3 + 2] = 0.0f; }
        v_idx++;
    }

    /* 4. Triangles */
    uint32_t i_idx = 0;
    for (uint32_t sec = 0; sec < S; sec++) {
        uint32_t b0 = body_start + sec;
        uint32_t b1 = b0 + 1;
        uint32_t t0 = body_start + (S + 1) + sec;
        uint32_t t1 = t0 + 1;

        i_ptr[i_idx++] = b0;
        i_ptr[i_idx++] = t0;
        i_ptr[i_idx++] = b1;

        i_ptr[i_idx++] = b1;
        i_ptr[i_idx++] = t0;
        i_ptr[i_idx++] = t1;
    }

    for (uint32_t sec = 0; sec < S; sec++) {
        i_ptr[i_idx++] = top_center;
        i_ptr[i_idx++] = top_rim + sec;
        i_ptr[i_idx++] = top_rim + sec + 1;
    }

    for (uint32_t sec = 0; sec < S; sec++) {
        i_ptr[i_idx++] = bot_center;
        i_ptr[i_idx++] = bot_rim + sec + 1;
        i_ptr[i_idx++] = bot_rim + sec;
    }

    *out_vert_count = v_count;
    *out_index_count = i_count;
    return true;
}

[[nodiscard]]
bool khr_scene_generate_chamfer_box(khr_bda_arena_t* arena,
                                    float half_size,
                                    float chamfer_size,
                                    VkDeviceAddress* out_verts,
                                    VkDeviceAddress* out_normals,
                                    VkDeviceAddress* out_indices,
                                    uint32_t* out_vert_count,
                                    uint32_t* out_index_count) {
    if (arena == nullptr || out_verts == nullptr || out_indices == nullptr ||
        out_vert_count == nullptr || out_index_count == nullptr) {
        return false;
    }
    float h = (half_size > 0.05f) ? half_size : 1.0f;
    float c = (chamfer_size > 0.005f && chamfer_size < h * 0.5f) ? chamfer_size : (h * 0.15f);
    float inner = h - c;

    // 24 vertices: 3 per corner (X, Y, Z face offsets)
    constexpr uint32_t v_count = 24;
    // 44 triangles (6 face quads + 12 edge bevel quads + 8 corner triangles) = 132 indices
    constexpr uint32_t i_count = 132;

    void* vhost = nullptr;
    void* nhost = nullptr;
    void* ihost = nullptr;
    VkDeviceAddress ngpu = 0;

    if (!khr_bda_arena_alloc(arena, v_count * 3 * sizeof(float), 16, &vhost, out_verts)) {
        return false;
    }
    if (out_normals != nullptr) {
        if (!khr_bda_arena_alloc(arena, v_count * 3 * sizeof(float), 16, &nhost, &ngpu)) {
            return false;
        }
        *out_normals = ngpu;
    }
    if (!khr_bda_arena_alloc(arena, i_count * sizeof(uint32_t), 16, &ihost, out_indices)) {
        return false;
    }

    float* v_ptr = (float*)vhost;
    float* n_ptr = (float*)nhost;
    uint32_t* i_ptr = (uint32_t*)ihost;

    // Corner signs: s[0]=x, s[1]=y, s[2]=z
    static const float signs[8][3] = {
        { -1.0f, -1.0f, -1.0f }, // 0
        {  1.0f, -1.0f, -1.0f }, // 1
        {  1.0f,  1.0f, -1.0f }, // 2
        { -1.0f,  1.0f, -1.0f }, // 3
        { -1.0f, -1.0f,  1.0f }, // 4
        {  1.0f, -1.0f,  1.0f }, // 5
        {  1.0f,  1.0f,  1.0f }, // 6
        { -1.0f,  1.0f,  1.0f }, // 7
    };

    for (uint32_t corner = 0; corner < 8; corner++) {
        float sx = signs[corner][0];
        float sy = signs[corner][1];
        float sz = signs[corner][2];

        // Vertex 0: Face X offset
        v_ptr[(corner * 3 + 0) * 3 + 0] = sx * h;
        v_ptr[(corner * 3 + 0) * 3 + 1] = sy * inner;
        v_ptr[(corner * 3 + 0) * 3 + 2] = sz * inner;

        // Vertex 1: Face Y offset
        v_ptr[(corner * 3 + 1) * 3 + 0] = sx * inner;
        v_ptr[(corner * 3 + 1) * 3 + 1] = sy * h;
        v_ptr[(corner * 3 + 1) * 3 + 2] = sz * inner;

        // Vertex 2: Face Z offset
        v_ptr[(corner * 3 + 2) * 3 + 0] = sx * inner;
        v_ptr[(corner * 3 + 2) * 3 + 1] = sy * inner;
        v_ptr[(corner * 3 + 2) * 3 + 2] = sz * h;

        if (n_ptr) {
            for (uint32_t k = 0; k < 3; k++) {
                uint32_t vi = corner * 3 + k;
                float nx = v_ptr[vi * 3 + 0];
                float ny = v_ptr[vi * 3 + 1];
                float nz = v_ptr[vi * 3 + 2];
                float len = sqrtf(nx * nx + ny * ny + nz * nz);
                if (len > 1e-5f) {
                    nx /= len; ny /= len; nz /= len;
                }
                n_ptr[vi * 3 + 0] = nx;
                n_ptr[vi * 3 + 1] = ny;
                n_ptr[vi * 3 + 2] = nz;
            }
        }
    }

    uint32_t ii = 0;
#define ADD_QUAD(a, b, c, d) khr_scene_add_quad(i_ptr, &ii, (a), (b), (c), (d))

    // 1. Six Main Faces
    // +X face (corners 1, 2, 6, 5, offset 0)
    ADD_QUAD(1 * 3 + 0, 2 * 3 + 0, 6 * 3 + 0, 5 * 3 + 0);
    // -X face (corners 0, 4, 7, 3, offset 0)
    ADD_QUAD(0 * 3 + 0, 4 * 3 + 0, 7 * 3 + 0, 3 * 3 + 0);
    // +Y face (corners 3, 2, 6, 7, offset 1)
    ADD_QUAD(3 * 3 + 1, 2 * 3 + 1, 6 * 3 + 1, 7 * 3 + 1);
    // -Y face (corners 0, 1, 5, 4, offset 1)
    ADD_QUAD(0 * 3 + 1, 1 * 3 + 1, 5 * 3 + 1, 4 * 3 + 1);
    // +Z face (corners 4, 5, 6, 7, offset 2)
    ADD_QUAD(4 * 3 + 2, 5 * 3 + 2, 6 * 3 + 2, 7 * 3 + 2);
    // -Z face (corners 0, 3, 2, 1, offset 2)
    ADD_QUAD(0 * 3 + 2, 3 * 3 + 2, 2 * 3 + 2, 1 * 3 + 2);

    // 2. Twelve Edge Bevel Quads
    ADD_QUAD(0 * 3 + 0, 1 * 3 + 0, 1 * 3 + 1, 0 * 3 + 1);
    ADD_QUAD(3 * 3 + 0, 2 * 3 + 0, 2 * 3 + 1, 3 * 3 + 1);
    ADD_QUAD(4 * 3 + 0, 5 * 3 + 0, 5 * 3 + 1, 4 * 3 + 1);
    ADD_QUAD(7 * 3 + 0, 6 * 3 + 0, 6 * 3 + 1, 7 * 3 + 1);

    ADD_QUAD(0 * 3 + 0, 4 * 3 + 0, 4 * 3 + 2, 0 * 3 + 2);
    ADD_QUAD(1 * 3 + 0, 5 * 3 + 0, 5 * 3 + 2, 1 * 3 + 2);
    ADD_QUAD(2 * 3 + 0, 6 * 3 + 0, 6 * 3 + 2, 2 * 3 + 2);
    ADD_QUAD(3 * 3 + 0, 7 * 3 + 0, 7 * 3 + 2, 3 * 3 + 2);

    ADD_QUAD(0 * 3 + 1, 3 * 3 + 1, 3 * 3 + 2, 0 * 3 + 2);
    ADD_QUAD(1 * 3 + 1, 2 * 3 + 1, 2 * 3 + 2, 1 * 3 + 2);
    ADD_QUAD(5 * 3 + 1, 6 * 3 + 1, 6 * 3 + 2, 5 * 3 + 2);
    ADD_QUAD(4 * 3 + 1, 7 * 3 + 1, 7 * 3 + 2, 4 * 3 + 2);

#undef ADD_QUAD

    // 3. Eight Corner Triangles
    for (uint32_t corner = 0; corner < 8; corner++) {
        uint32_t a = corner * 3 + 0;
        uint32_t b = corner * 3 + 1;
        uint32_t c = corner * 3 + 2;
        float sx = signs[corner][0];
        float sy = signs[corner][1];
        float sz = signs[corner][2];
        if (sx * sy * sz > 0.0f) {
            i_ptr[ii++] = a; i_ptr[ii++] = b; i_ptr[ii++] = c;
        } else {
            i_ptr[ii++] = a; i_ptr[ii++] = c; i_ptr[ii++] = b;
        }
    }

    *out_vert_count = v_count;
    *out_index_count = ii;
    return true;
}

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
                              uint32_t* out_index_count) {
    if (arena == nullptr || out_verts == nullptr || out_indices == nullptr ||
        out_vert_count == nullptr || out_index_count == nullptr) {
        return false;
    }
    float R = (major_radius > 0.1f) ? major_radius : 1.5f;
    float r = (minor_radius > 0.01f) ? minor_radius : 0.4f;
    uint32_t M = (major_segments >= 8) ? major_segments : 32;
    uint32_t N = (minor_segments >= 4) ? minor_segments : 16;

    uint32_t v_count = (M + 1) * (N + 1);
    uint32_t i_count = 6 * M * N;

    void* vhost = nullptr;
    void* nhost = nullptr;
    void* ihost = nullptr;
    VkDeviceAddress ngpu = 0;

    if (!khr_bda_arena_alloc(arena, (size_t)v_count * 3 * sizeof(float), 16, &vhost, out_verts)) {
        return false;
    }
    if (out_normals != nullptr) {
        if (!khr_bda_arena_alloc(arena, (size_t)v_count * 3 * sizeof(float), 16, &nhost, &ngpu)) {
            return false;
        }
        *out_normals = ngpu;
    }
    if (!khr_bda_arena_alloc(arena, (size_t)i_count * sizeof(uint32_t), 16, &ihost, out_indices)) {
        return false;
    }

    float* v_ptr = (float*)vhost;
    float* n_ptr = (float*)nhost;
    uint32_t* i_ptr = (uint32_t*)ihost;

    uint32_t vi = 0;
    for (uint32_t i = 0; i <= M; i++) {
        float u = (float)i * 2.0f * (float)M_PI / (float)M;
        float cos_u = cosf(u);
        float sin_u = sinf(u);

        for (uint32_t j = 0; j <= N; j++) {
            float v = (float)j * 2.0f * (float)M_PI / (float)N;
            float cos_v = cosf(v);
            float sin_v = sinf(v);

            v_ptr[vi * 3 + 0] = (R + r * cos_v) * cos_u;
            v_ptr[vi * 3 + 1] = r * sin_v;
            v_ptr[vi * 3 + 2] = (R + r * cos_v) * sin_u;

            if (n_ptr) {
                n_ptr[vi * 3 + 0] = cos_v * cos_u;
                n_ptr[vi * 3 + 1] = sin_v;
                n_ptr[vi * 3 + 2] = cos_v * sin_u;
            }
            vi++;
        }
    }

    uint32_t ii = 0;
    for (uint32_t i = 0; i < M; i++) {
        for (uint32_t j = 0; j < N; j++) {
            uint32_t p0 = i * (N + 1) + j;
            uint32_t p1 = p0 + 1;
            uint32_t p2 = (i + 1) * (N + 1) + j;
            uint32_t p3 = p2 + 1;

            i_ptr[ii++] = p0;
            i_ptr[ii++] = p2;
            i_ptr[ii++] = p1;

            i_ptr[ii++] = p1;
            i_ptr[ii++] = p2;
            i_ptr[ii++] = p3;
        }
    }

    *out_vert_count = v_count;
    *out_index_count = ii;
    return true;
}

void khr_scene_prepare_cull_push(const khr_scene_t* scene,
                                 const khr_camera_t* cam,
                                 khr_cull_push_t* out_push) {
    if (scene == nullptr || cam == nullptr || out_push == nullptr) {
        return;
    }
    *out_push = (khr_cull_push_t){
        .instances_addr = scene->instances_gpu,
        .culled_instances_addr = scene->culled_instances_gpu,
        .draw_cmd_addr = scene->draw_cmd_gpu,
        .draw_count_addr = scene->draw_count_gpu,
        .instance_count = scene->instance_count,
        .index_count = (scene->mesh_count > 0) ? scene->meshes[0].index_count : 0,
    };

    khr_camera_feed_cull_push(cam, out_push);

    /* Reset indirect command */
    if (scene->draw_cmd != nullptr) {
        *scene->draw_cmd = (khr_draw_indirect_cmd_t){
            .vertexCount = out_push->index_count,
            .instanceCount = 0,
            .firstVertex = 0,
            .firstInstance = 0,
        };
    }
    if (scene->draw_count != nullptr) {
        *scene->draw_count = 0;
    }
}

void khr_scene_prepare_hiz_cull_push(const khr_scene_t* scene,
                                     const khr_camera_t* cam,
                                     const khr_hiz_t* hiz,
                                     khr_hiz_cull_push_t* out_push) {
    if (scene == nullptr || cam == nullptr || hiz == nullptr || out_push == nullptr) {
        return;
    }

    uint32_t total_indices = 0;
    if (scene->mesh_count > 0) {
        total_indices = scene->meshes[0].index_count;
    }

    *out_push = (khr_hiz_cull_push_t){
        .instances_addr = scene->instances_gpu,
        .culled_instances_addr = scene->culled_instances_gpu,
        .draw_cmd_addr = scene->draw_cmd_gpu,
        .draw_count_addr = scene->draw_count_gpu,
        .instance_count = scene->instance_count,
        .index_count = total_indices,
        .alpha = 0.0f,
        .flags = 0x4U, /* bit 2: hiz enabled */
        .eye_fov = { cam->eye[0], cam->eye[1], cam->eye[2], cam->fov_y },
        .target_aspect = { cam->target[0], cam->target[1], cam->target[2], cam->aspect },
        .up_znear = { cam->up[0], cam->up[1], cam->up[2], cam->z_near },
        .visibility_addr = 0,
        .hiz_width = hiz->width,
        .hiz_height = hiz->height,
        .hiz_mips = hiz->mip_levels,
    };

    /* Reset indirect command */
    if (scene->draw_cmd != nullptr) {
        *scene->draw_cmd = (khr_draw_indirect_cmd_t){
            .vertexCount = out_push->index_count,
            .instanceCount = 0,
            .firstVertex = 0,
            .firstInstance = 0,
        };
    }
    if (scene->draw_count != nullptr) {
        *scene->draw_count = 0;
    }
}

void khr_scene_prepare_mesh_push(const khr_scene_t* scene,
                                 uint32_t mesh_id,
                                 const khr_camera_t* cam,
                                 khr_mesh_instanced_push_t* out_push) {
    if (scene == nullptr || cam == nullptr || out_push == nullptr) {
        return;
    }

    const khr_scene_mesh_t* mesh = nullptr;
    for (uint32_t i = 0; i < scene->mesh_count; i++) {
        if (scene->meshes[i].mesh_id == mesh_id) {
            mesh = &scene->meshes[i];
            break;
        }
    }
    if (mesh == nullptr && scene->mesh_count > 0) {
        mesh = &scene->meshes[0];
    }

    *out_push = (khr_mesh_instanced_push_t){
        .instances_addr = scene->culled_instances_gpu,
        .verts_addr = mesh != nullptr ? mesh->verts_addr : 0,
        .indices_addr = mesh != nullptr ? mesh->indices_addr : 0,
        .lights_addr = scene->lights_gpu,
        .light_dir = { 0.577f, 0.577f, 0.577f, 2.5f },
        .camera_pos = { cam->eye[0], cam->eye[1], cam->eye[2], 1.0f },
        .light_color = { 1.0f, 0.98f, 0.95f, 0.2f },
        .index_count = mesh != nullptr ? mesh->index_count : 0,
        .vert_count = mesh != nullptr ? mesh->vert_count : 0,
        .light_count = scene->light_count,
        .normals_addr = mesh != nullptr ? mesh->normals_addr : 0,
    };
}

// Khoros Engine - GPU Compute Frustum Culling & Math Pipeline
// Executes on GPU Compute Cores via Slang & SPIR-V 1.6.

#include "math.slang"

struct GpuInstance {
    float3 position;
    float  radius;
    float4 rotation;
    float3 scale;
    uint   mesh_id;
    float3 albedo;
    float  roughness;
    float  metallic;
    float  ao;
    uint   albedo_tex_id;
    uint   normal_tex_id;
};

struct GpuCulledInstance {
    Mat4   mvp;
    Mat4   model;
    float4 rotation;
    float3 scale;
    float  pad0;
    float3 albedo;
    float  roughness;
    float  metallic;
    float  ao;
    uint   albedo_tex_id;
    uint   normal_tex_id;
};

struct DrawIndirectCommand {
    uint vertexCount;
    uint instanceCount;
    uint firstVertex;
    uint firstInstance;
};

struct CullPush {
    GpuInstance*         instances;         // 8 bytes (offset 0)
    GpuCulledInstance*   culled_instances;  // 8 bytes (offset 8)
    DrawIndirectCommand* draw_cmd;          // 8 bytes (offset 16)
    uint*                draw_count;        // 8 bytes (offset 24)
    uint                 instance_count;    // 4 bytes (offset 32)
    uint                 index_count;       // 4 bytes (offset 36)
    float                alpha;             // 4 bytes (offset 40): render time alpha for tick interpolation
    uint                 flags;             // 4 bytes (offset 44): bit 0: interpolate, bit 1: prev_slot
    float4               eye_fov;           // 16 bytes (offset 48): eye.xyz, w = fov_y
    float4               target_aspect;     // 16 bytes (offset 64): target.xyz, w = aspect
    float4               up_znear;          // 16 bytes (offset 80): up.xyz, w = z_near
};

[[vk::push_constant]]
CullPush push;

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_main(uint32_t3 dtid : SV_DispatchThreadID) {
    uint idx = dtid.x;
    bool in_bounds = (idx < push.instance_count);
    bool visible = false;
    GpuCulledInstance culled = {};

    if (in_bounds) {
        float3 eye = push.eye_fov.xyz;
        float  fov_y = push.eye_fov.w;
        float3 target = push.target_aspect.xyz;
        float  aspect = push.target_aspect.w;
        float3 up = push.up_znear.xyz;
        float  z_near = push.up_znear.w;

        // 1. Compute Camera Matrices on GPU
        Mat4 view = mat4_look_at(eye, target, up);
        Mat4 proj = mat4_perspective_revz(fov_y, aspect, z_near);
        Mat4 vp = mat4_mul(proj, view);
        FrustumPlanes frustum = extract_frustum(vp);

        // 2. Read Instance Data via BDA with Sub-Frame Motion Interpolation
        GpuInstance inst = push.instances[idx];
        if ((push.flags & 1) != 0) {
            int prev_offset = ((push.flags & 2) != 0) ? -(int)push.instance_count : (int)push.instance_count;
            GpuInstance prev = push.instances[idx + prev_offset];
            float prev_s2 = dot(prev.scale, prev.scale);
            float prev_q2 = dot(prev.rotation, prev.rotation);
            if (prev_s2 > 0.001 && prev_q2 > 0.5 && prev_q2 < 2.0) {
                float a = clamp(push.alpha, 0.0, 1.0);
                inst.position = lerp(prev.position, inst.position, a);
                inst.rotation = quat_slerp(prev.rotation, inst.rotation, a);
                inst.scale = lerp(prev.scale, inst.scale, a);
            }
        }

        // 3. Compute Model & MVP Matrices on GPU
        Mat4 model = mat4_trs(inst.position, inst.rotation, inst.scale);
        Mat4 mvp = mat4_mul(vp, model);

        // 4. Frustum Culling Test on GPU
        float max_s = max(inst.scale.x, max(inst.scale.y, inst.scale.z));
        float world_radius = inst.radius * max_s;

        visible = cull_sphere(frustum, inst.position, world_radius);

        culled.mvp = mvp;
        culled.model = model;
        culled.rotation = inst.rotation;
        culled.scale = inst.scale;
        culled.pad0 = 0.0;
        culled.albedo = inst.albedo;
        culled.roughness = inst.roughness;
        culled.metallic = inst.metallic;
        culled.ao = inst.ao;
        culled.albedo_tex_id = inst.albedo_tex_id;
        culled.normal_tex_id = inst.normal_tex_id;
    }

    // Subgroup Wave Compaction: Compact atomic allocations across the SIMD wave (16/32/64 threads)
    // to eliminate atomic DRAM contention and L3 cache line bouncing on integrated GPUs/APUs.
    uint wave_count = WaveActiveCountBits(visible);
    uint prefix = WavePrefixCountBits(visible);
    uint wave_offset = 0;

    if (visible && prefix == 0) {
        InterlockedAdd(push.draw_cmd->instanceCount, wave_count, wave_offset);
        if (push.draw_count != nullptr) {
            InterlockedAdd(*push.draw_count, wave_count);
        }
    }

    uint base = WaveActiveBitOr(wave_offset);

    if (visible) {
        uint slot = base + prefix;
        push.culled_instances[slot] = culled;
    }
}

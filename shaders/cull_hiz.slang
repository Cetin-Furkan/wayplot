// Khoros Engine - GPU Compute Two-Pass Hi-Z Occlusion Culling & Frustum Pipeline
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

struct HiZCullPush {
    GpuInstance*         instances;         // 8 bytes (offset 0)
    GpuCulledInstance*   culled_instances;  // 8 bytes (offset 8)
    DrawIndirectCommand* draw_cmd;          // 8 bytes (offset 16)
    uint*                draw_count;        // 8 bytes (offset 24)
    uint                 instance_count;    // 4 bytes (offset 32)
    uint                 index_count;       // 4 bytes (offset 36)
    float                alpha;             // 4 bytes (offset 40)
    uint                 flags;             // 4 bytes (offset 44: bit 0: interp, bit 1: prev, bit 2: hiz, bit 3: late)
    float4               eye_fov;           // 16 bytes (offset 48)
    float4               target_aspect;     // 16 bytes (offset 64)
    float4               up_znear;          // 16 bytes (offset 80)
    uint*                visibility_mask;   // 8 bytes (offset 96)
    uint                 hiz_width;         // 4 bytes (offset 104)
    uint                 hiz_height;        // 4 bytes (offset 108)
    uint                 hiz_mips;          // 4 bytes (offset 112)
    uint                 pad;               // 4 bytes (offset 116)
    uint64_t             reserved;          // 8 bytes (offset 120)
};

[[vk::push_constant]]
HiZCullPush push;

[[vk::binding(0, 0)]]
Texture2D<float> hiz_pyramid;

[[vk::binding(1, 0)]]
SamplerState hiz_sampler;

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_main(uint32_t3 dtid : SV_DispatchThreadID) {
    uint idx = dtid.x;
    bool in_bounds = (idx < push.instance_count);
    bool visible = false;
    GpuCulledInstance culled = {};

    // In Late Pass (bit 3 of flags), skip instances already marked visible in early pass
    bool is_late_pass = ((push.flags & 8u) != 0u);
    if (in_bounds && is_late_pass && push.visibility_mask != nullptr) {
        if (push.visibility_mask[idx] != 0u) {
            return;
        }
    }

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
        if ((push.flags & 1u) != 0u) {
            int prev_offset = ((push.flags & 2u) != 0u) ? -(int)push.instance_count : (int)push.instance_count;
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

        // 5. Two-Pass Hi-Z Occlusion Culling Test
        bool enable_hiz = ((push.flags & 4u) != 0u) && (push.hiz_mips > 0u);
        if (visible && enable_hiz) {
            float3 center_v = view.mul_vec(float4(inst.position, 1.0)).xyz;
            float dist_fwd = -center_v.z;
            if (dist_fwd - world_radius >= z_near) {
                // Project 8 bounding box corners to screen space
                float3 bmin = inst.position - float3(world_radius, world_radius, world_radius);
                float3 bmax = inst.position + float3(world_radius, world_radius, world_radius);

                float2 min_uv = float2(1.0, 1.0);
                float2 max_uv = float2(0.0, 0.0);
                float max_depth = 0.0; // In reversed-Z, nearest depth is largest

                for (int i = 0; i < 8; i++) {
                    float3 corner = float3(
                        ((i & 1) != 0) ? bmax.x : bmin.x,
                        ((i & 2) != 0) ? bmax.y : bmin.y,
                        ((i & 4) != 0) ? bmax.z : bmin.z
                    );
                    float4 clip = vp.mul_vec(float4(corner, 1.0));
                    if (clip.w > 0.0) {
                        float3 ndc = clip.xyz / clip.w;
                        float2 uv = float2(ndc.x * 0.5 + 0.5, -ndc.y * 0.5 + 0.5);
                        min_uv = min(min_uv, uv);
                        max_uv = max(max_uv, uv);
                        max_depth = max(max_depth, ndc.z);
                    }
                }

                min_uv = clamp(min_uv, float2(0.0, 0.0), float2(1.0, 1.0));
                max_uv = clamp(max_uv, float2(0.0, 0.0), float2(1.0, 1.0));

                float2 pixel_size = (max_uv - min_uv) * float2(float(push.hiz_width), float(push.hiz_height));
                float max_pixel = max(pixel_size.x, pixel_size.y);
                float mip = ceil(log2(max(max_pixel, 1.0)));
                mip = clamp(mip, 0.0, float(push.hiz_mips - 1u));

                float2 center_uv = (min_uv + max_uv) * 0.5;
                float hiz_val = hiz_pyramid.SampleLevel(hiz_sampler, center_uv, mip).r;

                // In Reversed-Z: if max_depth (closest point) is less than hiz_val (farthest surface in tile),
                // the entire object is occluded behind rendered geometry.
                if (max_depth < hiz_val - 0.0001) {
                    visible = false;
                }
            }
        }

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

    // Update visibility mask for two-pass tracking
    if (in_bounds && push.visibility_mask != nullptr) {
        push.visibility_mask[idx] = visible ? 1u : 0u;
    }

    // Subgroup Wave Compaction
    uint wave_count = WaveActiveCountBits(visible);
    uint prefix = WavePrefixCountBits(visible);
    uint wave_offset = 0;

    if (visible && prefix == 0u) {
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

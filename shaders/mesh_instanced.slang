// Khoros Engine - GPU-Driven Instanced Mesh Rendering Pipeline with Cook-Torrance PBR
// Consumes GPU-culled instance matrices, material attributes, physical lights via BDA,
// and bindless textures via VK_EXT_descriptor_buffer (Pillar 1).

#include "math.slang"
#include "pbr.slang"

// Bindless Texture and Sampler Heaps (VK_EXT_descriptor_buffer)
[[vk::binding(0, 0)]]
Texture2D g_textures[];

[[vk::binding(0, 1)]]
SamplerState g_samplers[];

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

struct MeshInstancedPush {
    GpuCulledInstance* instances;    // 8 bytes (BDA) (offset 0)
    float*             verts;        // 8 bytes (packed xyz) (offset 8)
    uint*              indices;      // 8 bytes (uint32 triangle list) (offset 16)
    GpuLight*          lights;       // 8 bytes (BDA light buffer) (offset 24)
    float4             light_dir;    // 16 bytes: fallback dir.xyz, w = intensity (offset 32)
    float4             camera_pos;   // 16 bytes: xyz = camera eye (offset 48)
    float4             light_color;  // 16 bytes: rgb = light color, w = ambient factor (offset 64)
    uint               index_count;  // 4 bytes (offset 80)
    uint               vert_count;   // 4 bytes (offset 84)
    uint               light_count;  // 4 bytes (offset 88)
    uint               target_mesh_id; // 4 bytes (offset 92)
    float*             normals;      // 8 bytes (packed normal xyz) (offset 96)
    uint2              pad2;         // 8 bytes (offset 104) -> 112 bytes
};

[[vk::push_constant]]
MeshInstancedPush push;

struct VSOutput {
    float4 pos                 : SV_Position;
    float3 world_pos           : POSITION0;
    float3 normal              : NORMAL0;
    float3 albedo              : COLOR0;
    float3 pbr_params          : TEXCOORD0; // x: roughness, y: metallic, z: ao
    float2 uv                  : TEXCOORD1;
    nointerpolation uint albedo_tex_id : TEXCOORD2;
    nointerpolation uint normal_tex_id : TEXCOORD3;
};

float3 load_pos(uint i) {
    uint o = i * 3u;
    return float3(push.verts[o], push.verts[o + 1u], push.verts[o + 2u]);
}

[shader("vertex")]
VSOutput vs_main(uint vert_id : SV_VertexID, uint inst_id : SV_InstanceID) {
    VSOutput out;
    if (vert_id >= push.index_count || push.vert_count == 0u) {
        out.pos = float4(0.0, 0.0, 0.0, 1.0);
        out.world_pos = float3(0.0, 0.0, 0.0);
        out.normal = float3(0.0, 1.0, 0.0);
        out.albedo = float3(0.0, 0.0, 0.0);
        out.pbr_params = float3(0.5, 0.0, 1.0);
        out.uv = float2(0.0, 0.0);
        out.albedo_tex_id = 0xFFFFFFFFu;
        out.normal_tex_id = 0xFFFFFFFFu;
        return out;
    }

    uint tri = vert_id / 3u;
    uint corner = vert_id % 3u;
    uint ia = push.indices[tri * 3u + 0u];
    uint ib = push.indices[tri * 3u + 1u];
    uint ic = push.indices[tri * 3u + 2u];
    if (ia >= push.vert_count) ia = 0u;
    if (ib >= push.vert_count) ib = 0u;
    if (ic >= push.vert_count) ic = 0u;

    float3 p0 = load_pos(ia);
    float3 p1 = load_pos(ib);
    float3 p2 = load_pos(ic);
    float3 p = (corner == 0u) ? p0 : ((corner == 1u) ? p1 : p2);

    float3 n;
    if (push.normals != nullptr) {
        uint v_idx = (corner == 0u) ? ia : ((corner == 1u) ? ib : ic);
        uint no = v_idx * 3u;
        n = float3(push.normals[no], push.normals[no + 1u], push.normals[no + 2u]);
    } else {
        float3 e0 = p1 - p0;
        float3 e1 = p2 - p0;
        n = normalize(cross(e0, e1));
    }

    GpuCulledInstance inst = push.instances[inst_id];

    // Multi-mesh filtering and culling safety collapse:
    // If instance is culled (scale zero) or does not match target_mesh_id, collapse triangle to NDC (0,0,0)
    // with w=1.0 to guarantee zero rasterized area and prevent 0/0 NaN perspective division.
    if (dot(inst.scale, inst.scale) < 1e-8 || (push.target_mesh_id != 0xFFFFFFFFu && uint(inst.pad0 + 0.5) != push.target_mesh_id)) {
        out.pos = float4(0.0, 0.0, 0.0, 1.0);
        out.world_pos = float3(0.0, 0.0, 0.0);
        out.normal = float3(0.0, 1.0, 0.0);
        out.albedo = float3(0.0, 0.0, 0.0);
        out.pbr_params = float3(0.5, 0.0, 1.0);
        out.uv = float2(0.0, 0.0);
        out.albedo_tex_id = 0xFFFFFFFFu;
        out.normal_tex_id = 0xFFFFFFFFu;
        return out;
    }

    // Transform position to world space and clip space
    float4 p_world = inst.model.mul_vec(float4(p, 1.0));
    out.world_pos = p_world.xyz;

    out.pos = inst.mvp.mul_vec(float4(p, 1.0));
    out.pos.y = -out.pos.y; // Y-up mesh in Y-down framebuffer

    // Transform normal to world space using inverse scale and quaternion rotation:
    // Avoids skewing when scale is non-uniform (Sx != Sy != Sz)
    float3 n_world = normalize(quat_rotate(inst.rotation, n / max(inst.scale, 1e-5)));
    out.normal = n_world;

    // Pass PBR material attributes
    out.albedo = inst.albedo;
    out.pbr_params = float3(inst.roughness, inst.metallic, inst.ao);
    out.albedo_tex_id = inst.albedo_tex_id;
    out.normal_tex_id = inst.normal_tex_id;

    // Spherical UV parameterization for arbitrary 3D geometry
    float3 p_norm = normalize(p);
    float u = 0.5 + atan2(p_norm.z, p_norm.x) / (2.0 * 3.14159265359);
    float v = 0.5 - asin(clamp(p_norm.y, -1.0, 1.0)) / 3.14159265359;
    out.uv = float2(u, v);

    return out;
}

[shader("fragment")]
float4 fs_main(VSOutput pin) : SV_Target {
    float3 N = normalize(pin.normal);

    // Apply bindless normal map via triplanar projection if bound
    if (pin.normal_tex_id != 0xFFFFFFFFu) {
        float3 blend = abs(N);
        blend = blend / max(blend.x + blend.y + blend.z, 1e-4);
        float2 uv_x = pin.world_pos.yz * 0.75;
        float2 uv_y = pin.world_pos.xz * 0.75;
        float2 uv_z = pin.world_pos.xy * 0.75;
        float3 nx = g_textures[NonUniformResourceIndex(pin.normal_tex_id)].Sample(g_samplers[0], uv_x).rgb * 2.0 - 1.0;
        float3 ny = g_textures[NonUniformResourceIndex(pin.normal_tex_id)].Sample(g_samplers[0], uv_y).rgb * 2.0 - 1.0;
        float3 nz = g_textures[NonUniformResourceIndex(pin.normal_tex_id)].Sample(g_samplers[0], uv_z).rgb * 2.0 - 1.0;
        float3 perturbed = normalize(nx * blend.x + ny * blend.y + nz * blend.z);
        N = normalize(N + perturbed * 0.6);
    }

    // Camera view direction
    float3 cam_pos = push.camera_pos.xyz;
    float3 V = normalize(cam_pos - pin.world_pos);

    // Material parameters
    float3 albedo = pin.albedo;
    if (pin.albedo_tex_id != 0xFFFFFFFFu) {
        float3 blend = abs(N);
        blend = blend / max(blend.x + blend.y + blend.z, 1e-4);
        float2 uv_x = pin.world_pos.yz * 0.75;
        float2 uv_y = pin.world_pos.xz * 0.75;
        float2 uv_z = pin.world_pos.xy * 0.75;
        float4 cx = g_textures[NonUniformResourceIndex(pin.albedo_tex_id)].Sample(g_samplers[0], uv_x);
        float4 cy = g_textures[NonUniformResourceIndex(pin.albedo_tex_id)].Sample(g_samplers[0], uv_y);
        float4 cz = g_textures[NonUniformResourceIndex(pin.albedo_tex_id)].Sample(g_samplers[0], uv_z);
        float4 sampled_albedo = cx * blend.x + cy * blend.y + cz * blend.z;
        albedo *= sampled_albedo.rgb;
    }

    float roughness = clamp(pin.pbr_params.x, 0.045, 1.0);
    float metallic = clamp(pin.pbr_params.y, 0.0, 1.0);
    float ao = clamp(pin.pbr_params.z, 0.0, 1.0);

    // 1. Direct Lighting Evaluation (Physical BDA Lights or Fallback Key Light)
    float3 direct = float3(0.0, 0.0, 0.0);
    if (push.light_count > 0u && push.lights != nullptr) {
        direct = evaluate_all_lights(push.lights, push.light_count, pin.world_pos, N, V, albedo, roughness, metallic);
    } else {
        float3 L = normalize(push.light_dir.xyz);
        float intensity = push.light_dir.w > 0.0 ? push.light_dir.w : 2.5;
        float3 light_col = length(push.light_color.xyz) > 0.01 ? push.light_color.xyz : float3(1.0, 0.98, 0.95);
        float3 radiance = light_col * intensity;
        direct = pbr_direct_lighting(N, V, L, albedo, roughness, metallic, radiance);
    }

    // 2. Ambient Lighting (hemispherical sky/ground fill modulated by AO)
    float hemi = saturate(0.5 + 0.5 * N.y);
    float3 sky = float3(0.58, 0.64, 0.78);
    float3 gnd = float3(0.30, 0.24, 0.20);
    float3 amb_light = lerp(gnd, sky, hemi);
    float amb_factor = push.light_color.w > 0.0 ? push.light_color.w : 0.25;
    float3 ambient = albedo * amb_light * amb_factor * ao;

    // 3. Specular Environment Reflection (GGX split-sum approximation)
    float3 R = reflect(-V, N);
    float hemi_r = saturate(0.5 + 0.5 * R.y);
    float3 env_col = lerp(gnd, sky, hemi_r);
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float3 F_env = fresnel_schlick(saturate(dot(N, V)), F0);
    float3 specular_env = env_col * F_env * (1.0 - roughness * 0.7) * (metallic * 0.8 + 0.2);

    float3 color_hdr = direct + ambient + specular_env;

    // 4. ACES Filmic Tonemapping
    float3 color_ldr = tonemap_aces(color_hdr);

    // 5. Linear to sRGB Gamma Correction
    float3 color_srgb = linear_to_srgb(color_ldr);

    return float4(color_srgb, 1.0);
}

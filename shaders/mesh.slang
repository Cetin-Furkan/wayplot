// GPU-driven triangle mesh via BDA with Cook-Torrance GGX PBR & ACES Tonemapping.
#include "pbr.slang"

struct MeshPush
{
    float4 mvp_c0;
    float4 mvp_c1;
    float4 mvp_c2;
    float4 mvp_c3;
    float4 light_dir;
    float* verts;     // packed xyz, 3 floats per vertex
    uint*  indices;
    uint   index_count;
    uint   vert_count;
    uint   pad0;      // RGB8 tint if nonzero (gizmo arms & tests)
    uint   pad1;      // Material parameters / mode
};

[[vk::push_constant]]
MeshPush push;

struct VSOutput
{
    float4 pos        : SV_Position;
    float3 world_pos  : POSITION0;
    float3 normal     : NORMAL0;
    float4 tint_color : COLOR0;
    uint   is_tinted  : TEXCOORD0;
};

float4 xform_mvp(float4 v)
{
    return push.mvp_c0 * v.x + push.mvp_c1 * v.y + push.mvp_c2 * v.z + push.mvp_c3 * v.w;
}

float3 load_pos(uint i)
{
    uint o = i * 3u;
    return float3(push.verts[o], push.verts[o + 1u], push.verts[o + 2u]);
}

[shader("vertex")]
VSOutput vs_main(uint vert_id : SV_VertexID)
{
    VSOutput out;
    if (vert_id >= push.index_count || push.vert_count == 0u) {
        out.pos = float4(0.0, 0.0, 0.0, 1.0);
        out.world_pos = float3(0.0, 0.0, 0.0);
        out.normal = float3(0.0, 1.0, 0.0);
        out.tint_color = float4(0.0, 0.0, 0.0, 1.0);
        out.is_tinted = 0u;
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
    float3 e0 = p1 - p0;
    float3 e1 = p2 - p0;
    float3 n = normalize(cross(e0, e1));

    out.world_pos = p;
    out.normal = n;

    out.pos = xform_mvp(float4(p, 1.0));
    out.pos.y = -out.pos.y; /* Y-up mesh in a Y-down framebuffer */

    float3 l = normalize(push.light_dir.xyz);
    float ndl = max(dot(n, l), 0.0);

    if (push.pad0 != 0u) {
        float3 tint = float3(
            float(push.pad0 & 255u) * (1.0 / 255.0),
            float((push.pad0 >> 8u) & 255u) * (1.0 / 255.0),
            float((push.pad0 >> 16u) & 255u) * (1.0 / 255.0));
        out.tint_color = float4(tint * (0.42 + 0.58 * ndl), 1.0);
        out.is_tinted = 1u;
        return out;
    }

    out.tint_color = float4(0.0, 0.0, 0.0, 1.0);
    out.is_tinted = 0u;
    return out;
}

[shader("fragment")]
float4 fs_main(VSOutput pin) : SV_Target
{
    // If tinted (gizmo arm or depth test verification triangle), preserve exact test color
    if (pin.is_tinted != 0u) {
        return pin.tint_color;
    }

    // Modern Cook-Torrance GGX PBR Shading for Mesh / Suzanne
    float3 N = normalize(pin.normal);
    float3 L = normalize(push.light_dir.xyz);

    // Camera view vector (in object space)
    float3 V = normalize(float3(0.0, 0.0, 5.0) - pin.world_pos);

    // Physically based material properties:
    // Rich warm metallic gold/bronze finish with refined microfacet roughness
    float3 albedo = float3(1.0, 0.76, 0.33); // Metallic Gold base color
    float roughness = 0.25;                  // Microfacet specular glint
    float metallic = 0.92;                   // Real conductor (Cook-Torrance specular dominant)
    float ao = 1.0;                          // Ambient occlusion
    float3 radiance = float3(1.0, 0.98, 0.94) * 3.2; // Direct key light radiance

    // 1. Direct Cook-Torrance Specular + Diffuse Evaluation
    float3 direct = pbr_direct_lighting(N, V, L, albedo, roughness, metallic, radiance);

    // 2. Ambient Lighting (hemispherical fill modulated by AO)
    float hemi = saturate(0.5 + 0.5 * N.y);
    float3 sky = float3(0.55, 0.65, 0.85);
    float3 gnd = float3(0.25, 0.20, 0.18);
    float3 amb_light = lerp(gnd, sky, hemi);
    float3 ambient = albedo * amb_light * 0.35 * ao;

    float3 color_hdr = direct + ambient;

    // 3. ACES Filmic Tonemapping Curve
    float3 color_ldr = tonemap_aces(color_hdr);

    // 4. Linear to sRGB Gamma Correction
    float3 color_srgb = linear_to_srgb(color_ldr);

    return float4(color_srgb, 1.0);
}

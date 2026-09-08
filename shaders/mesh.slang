// GPU-driven triangle mesh via BDA. Packed float3 verts + uint32 indices.
// Face normals from the triangle (convex meshes need no depth buffer).

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
    uint   pad0;
    uint   pad1;
};

[[vk::push_constant]]
MeshPush push;

struct VSOutput
{
    float4 pos : SV_Position;
    float4 color : COLOR0;
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
        out.color = float4(0.0, 0.0, 0.0, 1.0);
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

    out.pos = xform_mvp(float4(p, 1.0));
    out.pos.y = -out.pos.y; /* Y-up mesh in a Y-down framebuffer */

    float3 l = normalize(push.light_dir.xyz);
    float ndl = max(dot(n, l), 0.0);
    if (push.pad0 != 0u) {
        float3 tint = float3(
            float(push.pad0 & 255u) * (1.0 / 255.0),
            float((push.pad0 >> 8u) & 255u) * (1.0 / 255.0),
            float((push.pad0 >> 16u) & 255u) * (1.0 / 255.0));
        out.color = float4(tint * (0.42 + 0.58 * ndl), 1.0);
        return out;
    }

    /* Per-face (not Phong). Hemisphere so no face is black, key for form. */
    float hemi = saturate(0.5 + 0.5 * n.y);
    float3 sky = float3(0.58, 0.64, 0.78);
    float3 gnd = float3(0.30, 0.24, 0.20);
    float3 amb = lerp(gnd, sky, hemi);
    float wrap = ndl * 0.5 + 0.5;
    float3 key = float3(1.00, 0.93, 0.84) * (0.18 + 0.62 * wrap);
    float3 base = float3(0.84, 0.78, 0.68);
    out.color = float4(base * (amb + key), 1.0);
    return out;
}

[shader("fragment")]
float4 fs_main(VSOutput pin) : SV_Target
{
    return pin.color;
}

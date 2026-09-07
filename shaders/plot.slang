// GPU-driven 3D ribbon plot shader via Buffer Device Address (BDA)
// Zero CPU vertex/index buffers. Procedural 3D ribbon extruded from raw float series.

struct PlotPush
{
    float4 mvp_c0;        // offset 0..15
    float4 mvp_c1;        // offset 16..31
    float4 mvp_c2;        // offset 32..47
    float4 mvp_c3;        // offset 48..63
    float4 light_dir;     // offset 64..79
    float* samples;       // offset 80..87 (8-byte aligned)
    uint count;           // offset 88..91 (4-byte aligned)
    float amp;            // offset 92..95 (4-byte aligned)
    float half_w;         // offset 96..99 (4-byte aligned)
    uint pad0;            // offset 100..103
    uint pad1;            // offset 104..107
    uint pad2;            // offset 108..111
};

[[vk::push_constant]]
PlotPush push;

struct VSOutput
{
    float4 pos : SV_Position;
    float4 color : COLOR0;
};

float3 calc_normal(uint idx, uint count, float amp, float sx)
{
    uint i0 = (idx > 0) ? idx - 1 : idx;
    uint i1 = (idx + 1 < count) ? idx + 1 : idx;
    float dx = float(i1 - i0) * sx;
    float y0 = push.samples[i0] * amp;
    float y1 = push.samples[i1] * amp;
    float dhdx = (dx > 1e-8) ? ((y1 - y0) / dx) : 0.0;
    return normalize(float3(-dhdx, 1.0, 0.0));
}

float4 calc_color(float sample_val)
{
    float tcol = clamp(sample_val, 0.0, 1.0);
    return float4(0.18, 0.40 + tcol * 0.45, 0.88, 1.0);
}

float4 xform_mvp(float4 v)
{
    return push.mvp_c0 * v.x + push.mvp_c1 * v.y + push.mvp_c2 * v.z + push.mvp_c3 * v.w;
}

[shader("vertex")]
VSOutput vs_main(uint vert_id : SV_VertexID, uint inst_id : SV_InstanceID)
{
    VSOutput out;

    uint seg = inst_id;
    uint count = push.count;
    float sx = 2.0 / float(count - 1);
    float amp = push.amp;
    float half_w = push.half_w;

    float x0 = -1.0 + float(seg) * sx;
    float x1 = -1.0 + float(seg + 1) * sx;
    float y0 = push.samples[seg] * amp;
    float y1 = push.samples[seg + 1] * amp;

    float3 n0 = calc_normal(seg, count, amp, sx);
    float3 n1 = calc_normal(seg + 1, count, amp, sx);

    float4 col0 = calc_color(push.samples[seg]);
    float4 col1 = calc_color(push.samples[seg + 1]);

    // 4 corners of the segment:
    // A: (x0, y0, -half_w) -> corner 0
    // B: (x1, y1, -half_w) -> corner 1
    // C: (x0, y0, +half_w) -> corner 2
    // D: (x1, y1, +half_w) -> corner 3
    static const uint corner_lut[12] = {
        0, 2, 3,  // Tri 1: A, C, D (top)
        0, 3, 1,  // Tri 2: A, D, B (top)
        0, 1, 3,  // Tri 3: A, B, D (underside)
        0, 3, 2   // Tri 4: A, D, C (underside)
    };

    uint corner = corner_lut[vert_id % 12u];
    bool is_underside = (vert_id >= 6u);

    float3 p;
    float3 norm;
    float4 base_col;

    if (corner == 0) {
        p = float3(x0, y0, -half_w);
        norm = n0;
        base_col = col0;
    } else if (corner == 1) {
        p = float3(x1, y1, -half_w);
        norm = n1;
        base_col = col1;
    } else if (corner == 2) {
        p = float3(x0, y0, half_w);
        norm = n0;
        base_col = col0;
    } else {
        p = float3(x1, y1, half_w);
        norm = n1;
        base_col = col1;
    }

    if (is_underside)
        norm = -norm;

    out.pos = xform_mvp(float4(p, 1.0));

    float3 l = normalize(-push.light_dir.xyz);
    float ndotl = abs(dot(norm, l));
    float light_factor = 0.2 + 0.8 * ndotl;

    out.color = float4(base_col.rgb * light_factor, 1.0);
    return out;
}

[shader("fragment")]
float4 fs_main(VSOutput pin) : SV_Target
{
    return pin.color;
}

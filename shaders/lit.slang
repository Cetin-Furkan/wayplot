struct VSIn {
    float3 pos : POSITION;
    float3 nrm : NORMAL;
    float3 col : COLOR;
    float2 uv : TEXCOORD;
};

struct VSOut {
    float4 pos : SV_Position;
    float3 nrm : NORMAL;
    float3 col : COLOR;
    float2 uv : TEXCOORD;
};

/* Four columns each, not float4x4: Slang/std140 has emitted RowMajor matrices
   even with -matrix-layout-column-major, which transposes the CPU clip matrix
   and collapses a cube into a dart. CPU writes column-major float[16]. */
struct UBO {
    float4 mvp_c0;
    float4 mvp_c1;
    float4 mvp_c2;
    float4 mvp_c3;
    float4 model_c0;
    float4 model_c1;
    float4 model_c2;
    float4 model_c3;
    float4 light_dir;
};

[[vk::binding(0, 0)]]
ConstantBuffer<UBO> u;

[[vk::binding(1, 0)]]
Sampler2D albedo;

float4 xform_cols(float4 c0, float4 c1, float4 c2, float4 c3, float4 v)
{
    return c0 * v.x + c1 * v.y + c2 * v.z + c3 * v.w;
}

[shader("vertex")]
VSOut vs_main(VSIn i)
{
    VSOut o;
    o.pos = xform_cols(u.mvp_c0, u.mvp_c1, u.mvp_c2, u.mvp_c3, float4(i.pos, 1.0));
    o.nrm = u.model_c0.xyz * i.nrm.x + u.model_c1.xyz * i.nrm.y + u.model_c2.xyz * i.nrm.z;
    o.col = i.col;
    o.uv = i.uv;
    return o;
}

[shader("fragment")]
float4 fs_main(VSOut i) : SV_Target
{
    float3 n = normalize(i.nrm);
    float3 l = normalize(-u.light_dir.xyz);
    float diff = saturate(dot(n, l)) * 0.82 + 0.18;
    float3 tex = albedo.Sample(i.uv).rgb;
    return float4(i.col * tex * diff, 1.0);
}

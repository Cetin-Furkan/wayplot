struct VSIn {
    float2 pos : POSITION;
    float2 uv : TEXCOORD0;
};

struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

struct UBO {
    float4 vp;    /* .xy = framebuffer pixels */
    float4 color;
};

[[vk::binding(0, 0)]]
ConstantBuffer<UBO> u;

[[vk::binding(1, 0)]]
Sampler2D atlas;

[shader("vertex")]
VSOut vs_main(VSIn i)
{
    VSOut o;
    float2 ndc = i.pos / u.vp.xy * 2.0 - 1.0;
    o.pos = float4(ndc, 0.0, 1.0);
    o.uv = i.uv;
    return o;
}

[shader("fragment")]
float4 fs_main(VSOut i) : SV_Target
{
    float a = atlas.Sample(i.uv).r;
    return float4(u.color.rgb, u.color.a * a);
}

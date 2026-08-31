struct VSIn {
    float2 pos : POSITION;
};

struct VSOut {
    float4 pos : SV_Position;
};

struct UBO {
    float4 vp;    /* .xy = framebuffer pixels */
    float4 color;
};

[[vk::binding(0, 0)]]
ConstantBuffer<UBO> u;

[shader("vertex")]
VSOut vs_main(VSIn i)
{
    VSOut o;
    float2 ndc = i.pos / u.vp.xy * 2.0 - 1.0;
    o.pos = float4(ndc, 0.0, 1.0);
    return o;
}

[shader("fragment")]
float4 fs_main(VSOut i) : SV_Target
{
    return u.color;
}

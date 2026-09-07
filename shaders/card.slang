// GPU-driven 2D Card/Panel shader via Buffer Device Address (BDA)
// Zero CPU vertex/index buffers. Procedural quad + analytical box SDF.

struct CardInstance
{
    float4 rect;          // x, y, w, h in pixel coordinates
    uint bg_rgba;         // Packed 32-bit RGBA8
    uint border_rgba;     // Packed 32-bit RGBA8
    float corner_radius;  // Corner radius in pixels
    float border_width;   // Border stroke width in pixels
};

struct CardPush
{
    CardInstance* cards;  // 64-bit GPU virtual address via BDA
    float2 screen_extent; // Viewport width, height in buffer pixels
    float scale;          // Buffer scale factor
    uint card_index;      // Base index offset
};

[[vk::push_constant]]
CardPush push;

struct VSOutput
{
    float4 pos : SV_Position;
    float2 local_pos : LOCAL_POS;
    float2 half_size : HALF_SIZE;
    float corner_radius : CORNER_RADIUS;
    float border_width : BORDER_WIDTH;
    float4 bg_color : BG_COLOR;
    float4 border_color : BORDER_COLOR;
};

float4 unpack_rgba(uint c)
{
    return float4(
        float(c & 0xFFu),
        float((c >> 8u) & 0xFFu),
        float((c >> 16u) & 0xFFu),
        float((c >> 24u) & 0xFFu)
    ) / 255.0;
}

[shader("vertex")]
VSOutput vs_main(uint vert_id : SV_VertexID, uint inst_id : SV_InstanceID)
{
    VSOutput out;
    CardInstance c = push.cards[push.card_index + inst_id];

    // Two triangles: (0, 1, 2) and (0, 2, 3)
    // TL, BL, BR, TR: CCW front-facing winding under Vulkan Y-down
    static const uint quad_indices[6] = { 0, 1, 2, 0, 2, 3 };
    static const float2 uvs[4] = {
        float2(0.0, 0.0), // 0: TL
        float2(0.0, 1.0), // 1: BL
        float2(1.0, 1.0), // 2: BR
        float2(1.0, 0.0)  // 3: TR
    };
    float2 uv = uvs[quad_indices[vert_id % 6u]];

    float2 pixel_pos = c.rect.xy + uv * c.rect.zw;
    out.pos = float4(2.0 * pixel_pos / push.screen_extent - 1.0, 0.0, 1.0);

    out.local_pos = (uv - 0.5) * c.rect.zw;
    out.half_size = c.rect.zw * 0.5;
    out.corner_radius = c.corner_radius;
    out.border_width = c.border_width;
    out.bg_color = unpack_rgba(c.bg_rgba);
    out.border_color = unpack_rgba(c.border_rgba);

    return out;
}

float sdRoundedBox(float2 p, float2 b, float r)
{
    float2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

[shader("fragment")]
float4 fs_main(VSOutput pin) : SV_Target
{
    // Exact polygon boundary for sharp unstyled cards (no AA fringe outside AABB)
    if (pin.corner_radius <= 0.0 && pin.border_width <= 0.0)
        return pin.bg_color;

    float r = min(pin.corner_radius, min(pin.half_size.x, pin.half_size.y));
    float d = sdRoundedBox(pin.local_pos, pin.half_size, r);

    // Subpixel anti-aliasing via screen-space derivative
    float aa = fwidth(d);
    float alpha = clamp(0.5 - d / max(aa, 1e-4), 0.0, 1.0);

    if (alpha <= 0.0)
        discard;

    float4 col = pin.bg_color;
    if (pin.border_width > 0.0)
    {
        float d_inner = d + pin.border_width;
        float inner_alpha = clamp(0.5 - d_inner / max(aa, 1e-4), 0.0, 1.0);
        col = lerp(pin.border_color, pin.bg_color, inner_alpha);
    }

    return float4(col.rgb, col.a * alpha);
}

// Khoros Engine - GPU-Driven Procedural PBR Texture Synthesizer
// Generates high-resolution PBR textures, tangent-space normals, and Damascus steel patterns
// directly in VRAM on the GPU compute cores with 0% CPU overhead.

struct TexSynthPush {
    uint   width;
    uint   height;
    uint   pattern;   // 0: Damascus Steel, 1: Brushed Metal, 2: Tangent Normal Map, 3: PBR Checker
    float  frequency;
    float4 tint;      // Base color tint / parameters
};

[[vk::push_constant]]
TexSynthPush push;

[[vk::binding(0, 0)]]
RWTexture2D<float4> out_tex;

// Pseudo-random hash on GPU
float hash12(float2 p) {
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float noise2d(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);

    float a = hash12(i + float2(0.0, 0.0));
    float b = hash12(i + float2(1.0, 0.0));
    float c = hash12(i + float2(0.0, 1.0));
    float d = hash12(i + float2(1.0, 1.0));

    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

float fbm(float2 p) {
    float v = 0.0;
    float a = 0.5;
    for (int i = 0; i < 4; i++) {
        v += a * noise2d(p);
        p *= 2.05;
        a *= 0.5;
    }
    return v;
}

[shader("compute")]
[numthreads(16, 16, 1)]
void cs_main(uint3 dtid : SV_DispatchThreadID) {
    if (dtid.x >= push.width || dtid.y >= push.height) {
        return;
    }

    float2 uv = (float2(dtid.xy) + 0.5) / float2(push.width, push.height);
    float freq = (push.frequency > 0.1) ? push.frequency : 8.0;
    float4 result = float4(1.0, 1.0, 1.0, 1.0);

    if (push.pattern == 0u) {
        // Pattern 0: Damascus Steel / Woven Metal Flow
        float2 p = uv * freq;
        float n1 = fbm(p + float2(0.0, 0.0));
        float n2 = fbm(p + float2(5.2, 1.3));
        float flow = sin((p.x + n1 * 4.0) * 12.0 + (p.y + n2 * 4.0) * 6.0);
        float bands = 0.5 + 0.5 * flow;
        bands = pow(bands, 1.8);

        float3 dark_steel  = float3(0.18, 0.20, 0.24);
        float3 light_steel = float3(0.85, 0.88, 0.92);
        float3 base_col = lerp(dark_steel, light_steel, bands);
        result = float4(base_col * push.tint.rgb, 1.0);
    }
    else if (push.pattern == 1u) {
        // Pattern 1: Brushed Anodized Metal (High-Frequency Micro-Grooves)
        float row = floor(uv.y * push.height * 0.5);
        float scratch = hash12(float2(row, 1.0));
        float groove = sin(uv.x * 300.0 * freq + scratch * 62.8) * 0.1;
        float grain = noise2d(float2(uv.x * 80.0, row * 0.2)) * 0.15;
        float val = 0.85 + groove + grain;
        result = float4(push.tint.rgb * val, 1.0);
    }
    else if (push.pattern == 2u) {
        // Pattern 2: GPU Tangent-Space Normal Map
        // Compute procedural height gradients and derive orthonormal normal vectors
        float2 p = uv * freq;
        float delta = 1.0 / float(push.width);
        float h_c = fbm(p);
        float h_r = fbm(p + float2(delta * freq, 0.0));
        float h_u = fbm(p + float2(0.0, delta * freq));

        float d_dx = (h_r - h_c) * 15.0;
        float d_dy = (h_u - h_c) * 15.0;
        float3 n = normalize(float3(-d_dx, -d_dy, 1.0));

        // Pack normal [-1, 1] into [0, 1] RGB
        result = float4(n * 0.5 + 0.5, 1.0);
    }
    else if (push.pattern == 3u) {
        // Pattern 3: PBR Calibration Checkerboard
        float2 check_pos = floor(uv * freq * 2.0);
        float check = fmod(check_pos.x + check_pos.y, 2.0);
        float3 col = (check > 0.5) ? push.tint.rgb : (push.tint.rgb * 0.2);
        result = float4(col, 1.0);
    }
    else if (push.pattern == 4u) {
        // Pattern 4: Veined Italian Carrara Marble / Polished Granite
        float2 p = uv * freq;
        float2 q = float2(fbm(p + float2(0.0, 0.0)), fbm(p + float2(5.2, 1.3)));
        float2 r = float2(fbm(p + 4.0 * q + float2(1.7, 9.2)), fbm(p + 4.0 * q + float2(8.3, 2.8)));
        float f = fbm(p + 4.0 * r);
        float vein = sin(p.x * 3.14159 + p.y * 2.0 + 7.0 * f);
        vein = pow(abs(vein), 0.75);

        float3 deep_vein = float3(0.12, 0.14, 0.18);
        float3 cream_stone = float3(0.95, 0.95, 0.97);
        float3 quartz_accent = float3(0.85, 0.82, 0.78);

        float3 marble_base = lerp(deep_vein, cream_stone, vein);
        marble_base = lerp(marble_base, quartz_accent, 0.25 * q.x);
        result = float4(marble_base * push.tint.rgb, 1.0);
    }
    else {
        // Pattern 5: Woven Carbon Fiber / Ballistic Kevlar
        float2 p = uv * freq * 16.0;
        float2 f_tile = floor(p);
        float2 f_sub = frac(p);
        float checker = fmod(f_tile.x + f_tile.y, 2.0);

        // Directional anisotropic highlight along weave direction
        float thread = (checker < 1.0) ? sin(f_sub.x * 3.14159) : sin(f_sub.y * 3.14159);
        thread = pow(clamp(thread, 0.0, 1.0), 0.5);

        float3 fiber_dark = float3(0.04, 0.04, 0.05);
        float3 fiber_glint = float3(0.35, 0.38, 0.45);
        float3 carbon = lerp(fiber_dark, fiber_glint, thread * 0.75);
        result = float4(carbon * push.tint.rgb, 1.0);
    }

    out_tex[dtid.xy] = result;
}

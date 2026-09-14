// Khoros Engine - Physically Based Rendering (PBR) Cook-Torrance Pipeline
// Microfacet Specular GGX, Smith Shadowing, Fresnel-Schlick, ACES Tonemapping.
// Real Radiometric Custom Lights: Directional, Point (Inverse-Square + Windowing), Spot (Penumbra).

static const float PI = 3.14159265358979323846;
static const float INV_PI = 0.31830988618379067154;

// GPU Light layout matching SPIR-V STD430 and khr_gpu_light_t (64 bytes)
struct GpuLight {
    float3 position;     // offset 0 (Point/Spot)
    uint   type;         // offset 12 (0 = Directional, 1 = Point, 2 = Spot)
    float3 direction;    // offset 16 (Directional/Spot)
    float  range;        // offset 28 (meters)
    float3 color;        // offset 32 (linear RGB)
    float  intensity;    // offset 44 (luminous intensity / illuminance)
    float  spot_inner;   // offset 48 (cos of inner cone half-angle)
    float  spot_outer;   // offset 52 (cos of outer cone half-angle)
    float2 pad;          // offset 56 (padding to 64 bytes)
};

// 1. Trowbridge-Reitz / GGX Normal Distribution Function (NDF)
// Models the relative surface area of microfacets oriented toward half-vector H.
float distribution_ggx(float3 N, float3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;

    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;
    return a2 / max(denom, 1.0e-7);
}

// 2. Schlick-GGX single-direction geometry shadowing/masking
float geometry_schlick_ggx(float NdotX, float k) {
    return NdotX / max(NdotX * (1.0 - k) + k, 1.0e-7);
}

// 3. Smith's method combining shadowing (L) and masking (V)
float geometry_smith(float3 N, float3 V, float3 L, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0; // Schlick-GGX direct lighting formulation
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx_v = geometry_schlick_ggx(NdotV, k);
    float ggx_l = geometry_schlick_ggx(NdotL, k);
    return ggx_v * ggx_l;
}

// 4. Fresnel-Schlick approximation
// Calculates the ratio of light reflected vs refracted at grazing angles.
float3 fresnel_schlick(float cos_theta, float3 F0) {
    float f = pow(clamp(1.0 - cos_theta, 0.0, 1.0), 5.0);
    return F0 + (float3(1.0, 1.0, 1.0) - F0) * f;
}

// 5. Full Cook-Torrance Microfacet BRDF Direct Lighting Evaluation
float3 pbr_direct_lighting(
    float3 N,
    float3 V,
    float3 L,
    float3 albedo,
    float  roughness,
    float  metallic,
    float3 radiance
) {
    float3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.001);

    // Dielectrics have F0 ~ 0.04; metals have F0 = albedo
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);

    // Microfacet terms
    float  D = distribution_ggx(N, H, roughness);
    float  G = geometry_smith(N, V, L, roughness);
    float3 F = fresnel_schlick(max(dot(H, V), 0.0), F0);

    // Specular Cook-Torrance lobe
    float3 numerator = D * G * F;
    float denominator = 4.0 * NdotV * NdotL + 0.0001;
    float3 specular = numerator / denominator;

    // Energy conservation: diffuse ratio kD = (1 - F) * (1 - metallic)
    float3 kS = F;
    float3 kD = (float3(1.0, 1.0, 1.0) - kS) * (1.0 - metallic);

    // Lambertian diffuse lobe
    float3 diffuse = kD * albedo * INV_PI;

    return (diffuse + specular) * radiance * NdotL;
}

// 6. Real Physically-Based Light Attenuation Evaluation (Pillar D)
// Evaluates inverse-square decay, Frostbite range windowing, and spotlight penumbra.
float evaluate_light_attenuation(GpuLight light, float3 P, out float3 out_L) {
    if (light.type == 0u) {
        // Directional Light: distance attenuation is infinite (constant irradiance)
        out_L = normalize(-light.direction);
        return 1.0;
    }

    // Point or Spot Light: compute vector from surface P to light source
    float3 to_light = light.position - P;
    float d = length(to_light);
    if (d >= light.range || d < 1.0e-5) {
        out_L = float3(0.0, 1.0, 0.0);
        return 0.0;
    }
    out_L = to_light / d;

    // Physically-based inverse square falloff: 1 / max(d^2, 0.0001)
    // Combined with Frostbite / Karis smooth windowing factor to reach 0 at radius R:
    // window = saturate(1 - (d / R)^4)^2
    float ratio = d / max(light.range, 0.001);
    float ratio2 = ratio * ratio;
    float ratio4 = ratio2 * ratio2;
    float f = saturate(1.0 - ratio4);
    float window = f * f;
    float att = window / max(d * d, 0.0001);

    // Spotlight Angular Attenuation
    if (light.type == 2u) {
        float cos_a = dot(-out_L, normalize(light.direction));
        float penumbra = saturate((cos_a - light.spot_outer) / max(light.spot_inner - light.spot_outer, 1.0e-4));
        att *= (penumbra * penumbra);
    }

    return att;
}

// 7. Multi-Light Radiance Accumulation via BDA
float3 evaluate_all_lights(
    GpuLight* lights,
    uint      light_count,
    float3    P,
    float3    N,
    float3    V,
    float3    albedo,
    float     roughness,
    float     metallic
) {
    float3 total_direct = float3(0.0, 0.0, 0.0);

    for (uint i = 0; i < light_count; i++) {
        GpuLight light = lights[i];
        float3 L;
        float att = evaluate_light_attenuation(light, P, L);
        if (att <= 0.0) {
            continue;
        }

        float3 radiance = light.color * (light.intensity * att);
        total_direct += pbr_direct_lighting(N, V, L, albedo, roughness, metallic, radiance);
    }

    return total_direct;
}

// 8. ACES Filmic Tonemapping Curve (Narkowicz 2015 fit)
// Maps high dynamic range [0, inf) into displayable [0, 1] with natural roll-off.
float3 tonemap_aces(float3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// 9. Linear to standard sRGB gamma transformation
float3 linear_to_srgb(float3 linear_color) {
    return pow(max(linear_color, float3(0.0, 0.0, 0.0)), float3(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2));
}

// Khoros Engine - Procedural Infinite Anti-Aliased Ground Grid Shader
// Executes on GPU Graphics Pipeline via Slang & SPIR-V 1.6 (Vulkan 1.4).

#include "math.slang"

struct GridPush {
    float4 eye_plane_y;     // xyz = camera eye pos, w = ground plane Y height
    float4 target_minor_sz; // xyz = camera target pos, w = minor grid cell size (e.g. 1.0)
    float4 up_major_sz;     // xyz = camera up vector, w = major grid cell size (e.g. 5.0)
    float4 params;          // x = fov_y, y = aspect, z = z_near, w = max_view_dist
};

[[vk::push_constant]]
GridPush push;

struct VsOutput {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

struct FsOutput {
    float4 color : SV_Target0;
    float  depth : SV_Depth;
};

[shader("vertex")]
VsOutput vs_main(uint vert_id : SV_VertexID) {
    VsOutput out;
    // Generate full-screen triangle covering [-1, 1] NDC
    float2 uvs[3] = {
        float2(-1.0, -1.0),
        float2( 3.0, -1.0),
        float2(-1.0,  3.0)
    };
    out.pos = float4(uvs[vert_id], 0.0, 1.0);
    out.uv  = uvs[vert_id];
    return out;
}

[shader("fragment")]
FsOutput fs_main(VsOutput in) {
    float3 eye    = push.eye_plane_y.xyz;
    float  plane_y = push.eye_plane_y.w;
    float3 target = push.target_minor_sz.xyz;
    float  minor_sz = push.target_minor_sz.w > 0.0 ? push.target_minor_sz.w : 1.0;
    float3 up     = push.up_major_sz.xyz;
    float  major_sz = push.up_major_sz.w > 0.0 ? push.up_major_sz.w : 5.0;

    float fov_y   = push.params.x;
    float aspect  = push.params.y;
    float z_near  = push.params.z;
    float max_dist = push.params.w > 0.0 ? push.params.w : 80.0;

    // 1. Build Camera Basis
    float3 f = normalize(target - eye);
    float3 r = normalize(cross(f, up));
    float3 u = cross(r, f);

    float half_h = tan(fov_y * 0.5);
    float half_w = half_h * aspect;

    // Vulkan NDC: in.uv.x in [-1, 1] maps to +r, in.uv.y in [-1, 1] maps to -u
    float3 ray_dir = normalize(f + in.uv.x * half_w * r - in.uv.y * half_h * u);

    static const float3 sky_color = float3(0.02, 0.04, 0.08);

    // 2. Intersect Ray with Ground Plane y = plane_y
    if (abs(ray_dir.y) < 1e-5) {
        FsOutput sky_out;
        sky_out.color = float4(sky_color, 1.0);
        sky_out.depth = 0.0;
        return sky_out;
    }

    float t = (plane_y - eye.y) / ray_dir.y;
    if (t <= 0.0) {
        FsOutput sky_out;
        sky_out.color = float4(sky_color, 1.0);
        sky_out.depth = 0.0;
        return sky_out;
    }

    float3 hit_pos = eye + ray_dir * t;
    float dist = length(hit_pos - eye);
    if (dist > max_dist) {
        FsOutput sky_out;
        sky_out.color = float4(sky_color, 1.0);
        sky_out.depth = 0.0;
        return sky_out;
    }

    // 3. Compute View-Projection and Reversed-Z Depth
    Mat4 view = mat4_look_at(eye, target, up);
    Mat4 proj = mat4_perspective_revz(fov_y, aspect, z_near);
    Mat4 vp   = mat4_mul(proj, view);
    float4 clip_pos = vp.mul_vec(float4(hit_pos, 1.0));

    float frag_depth = clip_pos.z / clip_pos.w;

    // 4. Anti-Aliased Grid Lines via Screen-Space Derivatives
    float2 coord = hit_pos.xz;
    float2 deriv = max(fwidth(coord), float2(1e-4, 1e-4));

    // Minor grid lines
    float2 minor_grid = abs(frac(coord / minor_sz - 0.5) - 0.5) / (deriv / minor_sz);
    float minor_line = min(minor_grid.x, minor_grid.y);
    float minor_alpha = 1.0 - min(minor_line, 1.0);

    // Major grid lines
    float2 major_grid = abs(frac(coord / major_sz - 0.5) - 0.5) / (deriv / major_sz);
    float major_line = min(major_grid.x, major_grid.y);
    float major_alpha = 1.0 - min(major_line, 1.0);

    // Coordinate Axes: X axis (hit_pos.z == 0) and Z axis (hit_pos.x == 0)
    float x_axis = 1.0 - min(abs(hit_pos.z) / deriv.y, 1.0);
    float z_axis = 1.0 - min(abs(hit_pos.x) / deriv.x, 1.0);

    // 5. Analytical Soft Contact Shadows & Sun Shadows from Scene Objects onto Floor
    // Pedestal at (0, -1.8, 0), Left Companion at (-3.5, 0, 0), Right Companion at (3.5, 0, 0)
    float shadow = 1.0;

    // Pedestal ambient contact shadow:
    float d_ped = length(hit_pos.xz - float2(0.0, 0.0));
    float occ_ped = 1.0 - smoothstep(1.3, 2.8, d_ped);
    shadow *= (1.0 - occ_ped * 0.78);

    // Companion left sphere ambient contact shadow:
    float d_left = length(hit_pos.xz - float2(-3.5, 0.0));
    float occ_left = 1.0 - smoothstep(0.8, 2.3, d_left);
    shadow *= (1.0 - occ_left * 0.65);

    // Companion right cube ambient contact shadow:
    float d_right = length(hit_pos.xz - float2(3.5, 0.0));
    float occ_right = 1.0 - smoothstep(0.8, 2.3, d_right);
    shadow *= (1.0 - occ_right * 0.65);

    // Directional Sunlight Shadow (Sun dir: -0.577, -0.577, -0.577, projecting towards +X, +Z):
    float2 sun_dir = normalize(float2(0.577, 0.577));
    float2 ped_sun = float2(0.0, 0.0) + sun_dir * 1.2;
    float sun_ped_sh = 1.0 - smoothstep(1.1, 2.9, length(hit_pos.xz - ped_sun));
    shadow *= (1.0 - sun_ped_sh * 0.45);

    float2 left_sun = float2(-3.5, 0.0) + sun_dir * 2.4;
    float sun_left_sh = 1.0 - smoothstep(0.9, 2.5, length(hit_pos.xz - left_sun));
    shadow *= (1.0 - sun_left_sh * 0.40);

    float2 right_sun = float2(3.5, 0.0) + sun_dir * 2.4;
    float sun_right_sh = 1.0 - smoothstep(0.9, 2.5, length(hit_pos.xz - right_sun));
    shadow *= (1.0 - sun_right_sh * 0.40);

    shadow = clamp(shadow, 0.12, 1.0);

    // 6. Shading Composition: Dark Architectural Slate Ground with Shadow Mask
    float4 col_minor = float4(0.40, 0.43, 0.50, 0.35) * minor_alpha;
    float4 col_major = float4(0.65, 0.70, 0.80, 0.75) * major_alpha;
    float4 col_xaxis = float4(0.95, 0.25, 0.25, 0.90) * x_axis; // Red X
    float4 col_zaxis = float4(0.25, 0.50, 0.95, 0.90) * z_axis; // Blue Z

    // Slate floor color modulated by physical ground shadows
    float3 floor_base = float3(0.09, 0.10, 0.13) * shadow;

    float3 color = floor_base;
    color = lerp(color, col_minor.rgb, col_minor.a);
    color = lerp(color, col_major.rgb, col_major.a);
    color = lerp(color, col_xaxis.rgb, col_xaxis.a);
    color = lerp(color, col_zaxis.rgb, col_zaxis.a);

    // Horizon Distance Fog Falloff
    float norm_dist = dist / max_dist;
    float fog = exp(-norm_dist * norm_dist * 3.5);

    color = lerp(sky_color, color, fog);

    FsOutput out;
    out.color = float4(color, 1.0);
    out.depth = frag_depth;
    return out;
}

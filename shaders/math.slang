// Khoros Engine - Pure GPU Math Library
// Executes on GPU Compute / Graphics Cores via Slang & SPIR-V 1.6.

#pragma once

struct Mat4 {
    float4 c0;
    float4 c1;
    float4 c2;
    float4 c3;

    float4 mul_vec(float4 v) {
        return c0 * v.x + c1 * v.y + c2 * v.z + c3 * v.w;
    }

    float4 row(int i) {
        if (i == 0) return float4(c0.x, c1.x, c2.x, c3.x);
        if (i == 1) return float4(c0.y, c1.y, c2.y, c3.y);
        if (i == 2) return float4(c0.z, c1.z, c2.z, c3.z);
        return float4(c0.w, c1.w, c2.w, c3.w);
    }
};

// Return 4x4 identity matrix
Mat4 mat4_identity() {
    Mat4 m;
    m.c0 = float4(1.0, 0.0, 0.0, 0.0);
    m.c1 = float4(0.0, 1.0, 0.0, 0.0);
    m.c2 = float4(0.0, 0.0, 1.0, 0.0);
    m.c3 = float4(0.0, 0.0, 0.0, 1.0);
    return m;
}

// 4x4 matrix multiplication A * B (column-major)
Mat4 mat4_mul(Mat4 a, Mat4 b) {
    Mat4 res;
    res.c0 = a.mul_vec(b.c0);
    res.c1 = a.mul_vec(b.c1);
    res.c2 = a.mul_vec(b.c2);
    res.c3 = a.mul_vec(b.c3);
    return res;
}

// Construct quaternion (x, y, z, w) from axis and angle (radians)
float4 quat_from_axis_angle(float3 axis, float angle_rad) {
    float half_a = angle_rad * 0.5;
    float s = sin(half_a);
    float c = cos(half_a);
    float3 norm_axis = normalize(axis);
    return float4(norm_axis * s, c);
}

// Hamilton quaternion multiplication P * Q
float4 quat_mul(float4 p, float4 q) {
    float3 pv = p.xyz;
    float pw = p.w;
    float3 qv = q.xyz;
    float qw = q.w;
    float3 v = pw * qv + qw * pv + cross(pv, qv);
    float w = pw * qw - dot(pv, qv);
    return float4(v, w);
}

// Rotate vector v by unit quaternion q
float3 quat_rotate(float4 q, float3 v) {
    float3 t = 2.0 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

// Spherical linear interpolation between two unit quaternions
float4 quat_slerp(float4 a, float4 b, float t) {
    float cos_half = dot(a, b);
    if (cos_half < 0.0) {
        b = -b;
        cos_half = -cos_half;
    }
    if (abs(cos_half) >= 1.0) {
        return a;
    }
    float half_theta = acos(clamp(cos_half, -1.0, 1.0));
    float sin_half = sqrt(max(0.0, 1.0 - cos_half * cos_half));
    if (abs(sin_half) < 0.001) {
        return normalize(lerp(a, b, t));
    }
    float ratio_a = sin((1.0 - t) * half_theta) / sin_half;
    float ratio_b = sin(t * half_theta) / sin_half;
    return a * ratio_a + b * ratio_b;
}

// Construct Model Matrix directly from Translation, Quaternion Rotation, and Scale
Mat4 mat4_trs(float3 t, float4 q, float3 s) {
    float x2 = q.x + q.x, y2 = q.y + q.y, z2 = q.z + q.z;
    float xx = q.x * x2,  xy = q.x * y2,  xz = q.x * z2;
    float yy = q.y * y2,  yz = q.y * z2,  zz = q.z * z2;
    float wx = q.w * x2,  wy = q.w * y2,  wz = q.w * z2;

    float3 r0 = float3(1.0 - (yy + zz), xy + wz, xz - wy);
    float3 r1 = float3(xy - wz, 1.0 - (xx + zz), yz + wx);
    float3 r2 = float3(xz + wy, yz - wx, 1.0 - (xx + yy));

    Mat4 m;
    m.c0 = float4(r0 * s.x, 0.0);
    m.c1 = float4(r1 * s.y, 0.0);
    m.c2 = float4(r2 * s.z, 0.0);
    m.c3 = float4(t, 1.0);
    return m;
}

// Construct LookAt View Matrix
Mat4 mat4_look_at(float3 eye, float3 target, float3 up) {
    float3 f = normalize(target - eye);
    float3 r = normalize(cross(f, up));
    float3 u = cross(r, f);

    Mat4 m;
    m.c0 = float4(r.x, u.x, -f.x, 0.0);
    m.c1 = float4(r.y, u.y, -f.y, 0.0);
    m.c2 = float4(r.z, u.z, -f.z, 0.0);
    m.c3 = float4(-dot(r, eye), -dot(u, eye), dot(f, eye), 1.0);
    return m;
}

// Vulkan Reversed-Z Perspective Projection with infinite far plane
// Maps Z_near -> 1.0, Z_far -> 0.0
Mat4 mat4_perspective_revz(float fov_y, float aspect, float z_near) {
    float g = 1.0 / tan(fov_y * 0.5);
    Mat4 m;
    m.c0 = float4(g / aspect, 0.0, 0.0, 0.0);
    m.c1 = float4(0.0, g, 0.0, 0.0);
    m.c2 = float4(0.0, 0.0, 0.0, -1.0);
    m.c3 = float4(0.0, 0.0, z_near, 0.0);
    return m;
}

struct FrustumPlanes {
    float4 planes[6]; // left, right, bottom, top, near, far
};

// Extract normalized frustum planes from View-Projection matrix
FrustumPlanes extract_frustum(Mat4 vp) {
    FrustumPlanes f;
    float4 r0 = vp.row(0);
    float4 r1 = vp.row(1);
    float4 r2 = vp.row(2);
    float4 r3 = vp.row(3);

    f.planes[0] = r3 + r0; // left:   w + x >= 0
    f.planes[1] = r3 - r0; // right:  w - x >= 0
    f.planes[2] = r3 + r1; // bottom: w + y >= 0
    f.planes[3] = r3 - r1; // top:    w - y >= 0
    f.planes[4] = r3 - r2; // near:   w - z >= 0 (Reversed-Z: z <= w)
    f.planes[5] = r2;      // far:    z >= 0     (Reversed-Z: z >= 0)

    for (int i = 0; i < 6; i++) {
        float len = length(f.planes[i].xyz);
        if (len > 1e-6) {
            f.planes[i] /= len;
        }
    }
    return f;
}

// Test sphere against frustum. Returns true if visible.
bool cull_sphere(FrustumPlanes f, float3 center, float radius) {
    for (int i = 0; i < 6; i++) {
        float d = dot(f.planes[i].xyz, center) + f.planes[i].w;
        if (d < -radius) {
            return false;
        }
    }
    return true;
}

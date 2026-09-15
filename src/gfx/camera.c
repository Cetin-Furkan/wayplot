#include "khoros/gfx/camera.h"
#include <math.h>
#include <string.h>

void khr_camera_init(khr_camera_t* cam, float fov_y_rad, float aspect, float z_near) {
    if (cam == nullptr) {
        return;
    }
    *cam = (khr_camera_t){
        .fov_y = fov_y_rad > 0.01f ? fov_y_rad : 1.04719755f, /* 60 deg default */
        .aspect = aspect > 0.01f ? aspect : 1.0f,
        .z_near = z_near > 0.001f ? z_near : 0.1f,
        .z_far = 1.0e6f,
        .radius = 5.0f,
        .yaw = 0.0f,
        .pitch = 0.0f,
        .target = { 0.0f, 0.0f, 0.0f },
        .up = { 0.0f, 1.0f, 0.0f },
        .eye = { 0.0f, 0.0f, 5.0f },
        .quat = { 0.0f, 0.0f, 0.0f, 1.0f },
    };
}

void khr_camera_look_at(khr_camera_t* cam, const float eye[3], const float target[3], const float up[3]) {
    if (cam == nullptr || eye == nullptr || target == nullptr || up == nullptr) {
        return;
    }
    cam->eye[0] = eye[0];
    cam->eye[1] = eye[1];
    cam->eye[2] = eye[2];
    cam->target[0] = target[0];
    cam->target[1] = target[1];
    cam->target[2] = target[2];
    cam->up[0] = up[0];
    cam->up[1] = up[1];
    cam->up[2] = up[2];

    float dx = eye[0] - target[0];
    float dy = eye[1] - target[1];
    float dz = eye[2] - target[2];
    float r = sqrtf(dx * dx + dy * dy + dz * dz);
    cam->radius = r > 1.0e-5f ? r : 1.0e-5f;

    cam->yaw = atan2f(dx, dz);
    float d_xz = sqrtf(dx * dx + dz * dz);
    cam->pitch = atan2f(dy, d_xz);
}

void khr_camera_orbit(khr_camera_t* cam, float dyaw, float dpitch) {
    if (cam == nullptr) {
        return;
    }
    cam->yaw += dyaw;
    cam->pitch += dpitch;

    /* Pitch clamping to avoid pole singularities */
    constexpr float PITCH_LIMIT = 1.553343f; /* ~89 degrees */
    if (cam->pitch > PITCH_LIMIT) {
        cam->pitch = PITCH_LIMIT;
    } else if (cam->pitch < -PITCH_LIMIT) {
        cam->pitch = -PITCH_LIMIT;
    }

    float cp = cosf(cam->pitch);
    float sp = sinf(cam->pitch);
    float cy = cosf(cam->yaw);
    float sy = sinf(cam->yaw);

    cam->eye[0] = cam->target[0] + cam->radius * sy * cp;
    cam->eye[1] = cam->target[1] + cam->radius * sp;
    cam->eye[2] = cam->target[2] + cam->radius * cy * cp;
}

/* Map 2D point in [-1, 1] to 3D unit sphere point */
static void khr_arcball_project_to_sphere(float x, float y, float out[3]) {
    float r2 = x * x + y * y;
    if (r2 <= 1.0f) {
        out[0] = x;
        out[1] = y;
        out[2] = sqrtf(1.0f - r2);
    } else {
        float r = sqrtf(r2);
        out[0] = x / r;
        out[1] = y / r;
        out[2] = 0.0f;
    }
}

void khr_camera_arcball(khr_camera_t* cam, float p0_x, float p0_y, float p1_x, float p1_y) {
    if (cam == nullptr) {
        return;
    }
    float v0[3] = {};
    float v1[3] = {};
    khr_arcball_project_to_sphere(p0_x, p0_y, v0);
    khr_arcball_project_to_sphere(p1_x, p1_y, v1);

    /* Rotation axis = v0 x v1 */
    float axis[3] = {
        v0[1] * v1[2] - v0[2] * v1[1],
        v0[2] * v1[0] - v0[0] * v1[2],
        v0[0] * v1[1] - v0[1] * v1[0],
    };
    float axis_len = sqrtf(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    if (axis_len < 1.0e-6f) {
        return;
    }

    axis[0] /= axis_len;
    axis[1] /= axis_len;
    axis[2] /= axis_len;

    float dot = v0[0] * v1[0] + v0[1] * v1[1] + v0[2] * v1[2];
    if (dot > 1.0f) dot = 1.0f;
    if (dot < -1.0f) dot = -1.0f;
    float angle = acosf(dot);

    /* Construct delta quaternion */
    float s = sinf(angle * 0.5f);
    float c = cosf(angle * 0.5f);
    float dq[4] = { axis[0] * s, axis[1] * s, axis[2] * s, c };

    /* Rotate vector (eye - target) by dq: v' = dq * v * dq^-1 */
    float v[3] = {
        cam->eye[0] - cam->target[0],
        cam->eye[1] - cam->target[1],
        cam->eye[2] - cam->target[2],
    };

    /* Rodriguez / quaternion rotation: v' = v + 2*cross(dq.xyz, cross(dq.xyz, v) + dq.w*v) */
    float t[3] = {
        2.0f * (dq[1] * v[2] - dq[2] * v[1]),
        2.0f * (dq[2] * v[0] - dq[0] * v[2]),
        2.0f * (dq[0] * v[1] - dq[1] * v[0]),
    };
    float v_rot[3] = {
        v[0] + dq[3] * t[0] + (dq[1] * t[2] - dq[2] * t[1]),
        v[1] + dq[3] * t[1] + (dq[2] * t[0] - dq[0] * t[2]),
        v[2] + dq[3] * t[2] + (dq[0] * t[1] - dq[1] * t[0]),
    };

    cam->eye[0] = cam->target[0] + v_rot[0];
    cam->eye[1] = cam->target[1] + v_rot[1];
    cam->eye[2] = cam->target[2] + v_rot[2];

    /* Update current quaternion */
    float q_cur[4] = { cam->quat[0], cam->quat[1], cam->quat[2], cam->quat[3] };
    cam->quat[0] = dq[3] * q_cur[0] + dq[0] * q_cur[3] + dq[1] * q_cur[2] - dq[2] * q_cur[1];
    cam->quat[1] = dq[3] * q_cur[1] - dq[0] * q_cur[2] + dq[1] * q_cur[3] + dq[2] * q_cur[0];
    cam->quat[2] = dq[3] * q_cur[2] + dq[0] * q_cur[1] - dq[1] * q_cur[0] + dq[2] * q_cur[3];
    cam->quat[3] = dq[3] * q_cur[3] - dq[0] * q_cur[0] - dq[1] * q_cur[1] - dq[2] * q_cur[2];

    /* Normalize quaternion */
    float qlen = sqrtf(cam->quat[0]*cam->quat[0] + cam->quat[1]*cam->quat[1] +
                       cam->quat[2]*cam->quat[2] + cam->quat[3]*cam->quat[3]);
    if (qlen > 1.0e-6f) {
        cam->quat[0] /= qlen;
        cam->quat[1] /= qlen;
        cam->quat[2] /= qlen;
        cam->quat[3] /= qlen;
    }
}

void khr_camera_pan(khr_camera_t* cam, float dx, float dy) {
    if (cam == nullptr) {
        return;
    }
    float f[3] = {
        cam->target[0] - cam->eye[0],
        cam->target[1] - cam->eye[1],
        cam->target[2] - cam->eye[2],
    };
    float flen = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (flen < 1.0e-5f) return;
    f[0] /= flen; f[1] /= flen; f[2] /= flen;

    /* Right = f x up */
    float r[3] = {
        f[1] * cam->up[2] - f[2] * cam->up[1],
        f[2] * cam->up[0] - f[0] * cam->up[2],
        f[0] * cam->up[1] - f[1] * cam->up[0],
    };
    float rlen = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (rlen < 1.0e-5f) return;
    r[0] /= rlen; r[1] /= rlen; r[2] /= rlen;

    /* Up_true = r x f */
    float u[3] = {
        r[1] * f[2] - r[2] * f[1],
        r[2] * f[0] - r[0] * f[2],
        r[0] * f[1] - r[1] * f[0],
    };

    float scale = cam->radius * 0.002f;
    float ox = (r[0] * dx + u[0] * dy) * scale;
    float oy = (r[1] * dx + u[1] * dy) * scale;
    float oz = (r[2] * dx + u[2] * dy) * scale;

    cam->eye[0] += ox;
    cam->eye[1] += oy;
    cam->eye[2] += oz;
    cam->target[0] += ox;
    cam->target[1] += oy;
    cam->target[2] += oz;
}

void khr_camera_zoom(khr_camera_t* cam, float delta) {
    if (cam == nullptr || delta == 0.0f) {
        return;
    }
    float factor = expf(-delta * 0.12f);
    if (factor < 0.05f) factor = 0.05f;
    if (factor > 20.0f) factor = 20.0f;

    cam->radius *= factor;
    if (cam->radius < 0.25f) cam->radius = 0.25f;
    if (cam->radius > 1.0e5f) cam->radius = 1.0e5f;

    float f[3] = {
        cam->eye[0] - cam->target[0],
        cam->eye[1] - cam->target[1],
        cam->eye[2] - cam->target[2],
    };
    float flen = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (flen < 1.0e-5f) return;

    cam->eye[0] = cam->target[0] + (f[0] / flen) * cam->radius;
    cam->eye[1] = cam->target[1] + (f[1] / flen) * cam->radius;
    cam->eye[2] = cam->target[2] + (f[2] / flen) * cam->radius;
}

void khr_camera_set_aspect(khr_camera_t* cam, float aspect) {
    if (cam == nullptr) {
        return;
    }
    cam->aspect = aspect > 0.01f ? aspect : 1.0f;
}

void khr_camera_fit_aabb(khr_camera_t* cam, const float min_p[3], const float max_p[3]) {
    if (cam == nullptr || min_p == nullptr || max_p == nullptr) {
        return;
    }
    cam->target[0] = 0.5f * (min_p[0] + max_p[0]);
    cam->target[1] = 0.5f * (min_p[1] + max_p[1]);
    cam->target[2] = 0.5f * (min_p[2] + max_p[2]);

    float dx = max_p[0] - min_p[0];
    float dy = max_p[1] - min_p[1];
    float dz = max_p[2] - min_p[2];
    float diag = sqrtf(dx * dx + dy * dy + dz * dz);
    if (diag < 1.0e-4f) diag = 1.0e-4f;

    float half_fov = cam->fov_y * 0.5f;
    cam->radius = (diag * 0.5f) / sinf(half_fov);
    if (cam->radius < 0.1f) cam->radius = 0.1f;

    /* Position eye at target + radius * (0, 0, 1) */
    cam->eye[0] = cam->target[0];
    cam->eye[1] = cam->target[1];
    cam->eye[2] = cam->target[2] + cam->radius;
    cam->yaw = 0.0f;
    cam->pitch = 0.0f;
}

void khr_camera_feed_cull_push(const khr_camera_t* cam, khr_cull_push_t* push) {
    if (cam == nullptr || push == nullptr) {
        return;
    }
    push->eye_fov[0] = cam->eye[0];
    push->eye_fov[1] = cam->eye[1];
    push->eye_fov[2] = cam->eye[2];
    push->eye_fov[3] = cam->fov_y;

    push->target_aspect[0] = cam->target[0];
    push->target_aspect[1] = cam->target[1];
    push->target_aspect[2] = cam->target[2];
    push->target_aspect[3] = cam->aspect;

    push->up_znear[0] = cam->up[0];
    push->up_znear[1] = cam->up[1];
    push->up_znear[2] = cam->up[2];
    push->up_znear[3] = cam->z_near;
}

void khr_camera_get_rotation_matrix(const khr_camera_t* cam, float r0[3], float r1[3], float r2[3]) {
    if (cam == nullptr || r0 == nullptr || r1 == nullptr || r2 == nullptr) {
        return;
    }
    float cy = cosf(cam->yaw);
    float sy = sinf(cam->yaw);
    float cx = cosf(cam->pitch);
    float sx = sinf(cam->pitch);
    r0[0] = cy;
    r0[1] = 0.0f;
    r0[2] = -sy;
    r1[0] = sy * sx;
    r1[1] = cx;
    r1[2] = cy * sx;
    r2[0] = sy * cx;
    r2[1] = -sx;
    r2[2] = cy * cx;
}

void khr_camera_snap_axis(khr_camera_t* cam, int axis) {
    if (cam == nullptr || axis == 0) {
        return;
    }
    if (axis == 1) {
        cam->yaw = -1.5707963f;
        cam->pitch = 0.0f;
    } else if (axis == -1) {
        cam->yaw = 1.5707963f;
        cam->pitch = 0.0f;
    } else if (axis == 2) {
        cam->pitch = 1.52f;
    } else if (axis == -2) {
        cam->pitch = -1.52f;
    } else if (axis == 3) {
        cam->yaw = 3.14159265f;
        cam->pitch = 0.0f;
    } else if (axis == -3) {
        cam->yaw = 0.0f;
        cam->pitch = 0.0f;
    }
    float cp = cosf(cam->pitch);
    float sp = sinf(cam->pitch);
    float cy = cosf(cam->yaw);
    float sy = sinf(cam->yaw);
    cam->eye[0] = cam->target[0] + cam->radius * sy * cp;
    cam->eye[1] = cam->target[1] + cam->radius * sp;
    cam->eye[2] = cam->target[2] + cam->radius * cy * cp;
}

int khr_camera_pick_axis(const khr_camera_t* cam, float nx, float ny) {
    if (cam == nullptr) {
        return 0;
    }
    float r0[3] = {};
    float r1[3] = {};
    float r2[3] = {};
    khr_camera_get_rotation_matrix(cam, r0, r1, r2);
    float r2_len = nx * nx + ny * ny;
    if (r2_len < 0.12f) {
        float ax = r0[2];
        float ay = r1[2];
        float az = r2[2];
        float mx = ax < 0.0f ? -ax : ax;
        float my = ay < 0.0f ? -ay : ay;
        float mz = az < 0.0f ? -az : az;
        if (mx >= my && mx >= mz && mx > 0.35f) {
            return ax < 0.0f ? 1 : -1;
        }
        if (my >= mx && my >= mz && my > 0.35f) {
            return ay < 0.0f ? 2 : -2;
        }
        if (mz > 0.35f) {
            return az < 0.0f ? 3 : -3;
        }
        return 0;
    }
    constexpr float k = 0.78f;
    const float tips[6][3] = {
        {  1.0f,  r0[0] * k, -r0[1] * k },
        { -1.0f, -r0[0] * k,  r0[1] * k },
        {  2.0f,  r1[0] * k, -r1[1] * k },
        { -2.0f, -r1[0] * k,  r1[1] * k },
        {  3.0f,  r2[0] * k, -r2[1] * k },
        { -3.0f, -r2[0] * k,  r2[1] * k },
    };
    float best_d = 0.42f * 0.42f;
    int best = 0;
    for (int i = 0; i < 6; i++) {
        float dx = nx - tips[i][1];
        float dy = ny - tips[i][2];
        float d = dx * dx + dy * dy;
        if (d < best_d) {
            best_d = d;
            best = (int)tips[i][0];
        }
    }
    return best;
}

void khr_camera_frame_verts(khr_camera_t* cam, const float* xyz, uint32_t nv, bool reset_orient) {
    if (cam == nullptr) {
        return;
    }
    if (reset_orient) {
        cam->yaw = 0.55f + 3.14159265f;
        cam->pitch = 0.38f;
    }
    cam->target[0] = 0.0f;
    cam->target[1] = 0.0f;
    cam->target[2] = 0.0f;
    cam->radius = 1.0f;
    if (xyz != nullptr && nv > 0) {
        float mn[3] = { xyz[0], xyz[1], xyz[2] };
        float mx[3] = { xyz[0], xyz[1], xyz[2] };
        for (uint32_t i = 1; i < nv; i++) {
            const float* p = xyz + i * 3U;
            for (int a = 0; a < 3; a++) {
                if (p[a] < mn[a]) mn[a] = p[a];
                if (p[a] > mx[a]) mx[a] = p[a];
            }
        }
        cam->target[0] = 0.5f * (mn[0] + mx[0]);
        cam->target[1] = 0.5f * (mn[1] + mx[1]);
        cam->target[2] = 0.5f * (mn[2] + mx[2]);
        float dx = mx[0] - mn[0];
        float dy = mx[1] - mn[1];
        float dz = mx[2] - mn[2];
        float diag = sqrtf(dx * dx + dy * dy + dz * dz);
        if (diag < 1.0e-6f) {
            diag = 1.0e-6f;
        }
        float half_fov = cam->fov_y * 0.5f;
        cam->radius = (diag * 0.5f) / sinf(half_fov);
        if (cam->radius < 0.1f) {
            cam->radius = 0.1f;
        }
    }
    float cp = cosf(cam->pitch);
    float sp = sinf(cam->pitch);
    float cy = cosf(cam->yaw);
    float sy = sinf(cam->yaw);
    cam->eye[0] = cam->target[0] + cam->radius * sy * cp;
    cam->eye[1] = cam->target[1] + cam->radius * sp;
    cam->eye[2] = cam->target[2] + cam->radius * cy * cp;
}

void khr_camera_screen_to_ray(const khr_camera_t* cam,
                              float screen_x, float screen_y,
                              float viewport_w, float viewport_h,
                              float out_origin[3], float out_dir[3]) {
    if (cam == nullptr || out_origin == nullptr || out_dir == nullptr) {
        return;
    }
    out_origin[0] = cam->eye[0];
    out_origin[1] = cam->eye[1];
    out_origin[2] = cam->eye[2];

    float w = (viewport_w > 0.0f) ? viewport_w : 960.0f;
    float h = (viewport_h > 0.0f) ? viewport_h : 540.0f;

    float ndc_x = (2.0f * screen_x / w) - 1.0f;
    float ndc_y = 1.0f - (2.0f * screen_y / h);

    float fwd[3] = {
        cam->target[0] - cam->eye[0],
        cam->target[1] - cam->eye[1],
        cam->target[2] - cam->eye[2],
    };
    float flen = sqrtf(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
    if (flen > 1e-7f) {
        fwd[0] /= flen; fwd[1] /= flen; fwd[2] /= flen;
    } else {
        fwd[0] = 0.0f; fwd[1] = 0.0f; fwd[2] = -1.0f;
    }

    float rgt[3] = {
        fwd[1] * cam->up[2] - fwd[2] * cam->up[1],
        fwd[2] * cam->up[0] - fwd[0] * cam->up[2],
        fwd[0] * cam->up[1] - fwd[1] * cam->up[0],
    };
    float rlen = sqrtf(rgt[0] * rgt[0] + rgt[1] * rgt[1] + rgt[2] * rgt[2]);
    if (rlen > 1e-7f) {
        rgt[0] /= rlen; rgt[1] /= rlen; rgt[2] /= rlen;
    } else {
        rgt[0] = 1.0f; rgt[1] = 0.0f; rgt[2] = 0.0f;
    }

    float u[3] = {
        rgt[1] * fwd[2] - rgt[2] * fwd[1],
        rgt[2] * fwd[0] - rgt[0] * fwd[2],
        rgt[0] * fwd[1] - rgt[1] * fwd[0],
    };

    float tan_fov = tanf(cam->fov_y * 0.5f);
    float rx = ndc_x * cam->aspect * tan_fov;
    float ry = ndc_y * tan_fov;

    float rd[3] = {
        fwd[0] + rgt[0] * rx + u[0] * ry,
        fwd[1] + rgt[1] * rx + u[1] * ry,
        fwd[2] + rgt[2] * rx + u[2] * ry,
    };
    float rd_len = sqrtf(rd[0] * rd[0] + rd[1] * rd[1] + rd[2] * rd[2]);
    if (rd_len > 1e-7f) {
        out_dir[0] = rd[0] / rd_len;
        out_dir[1] = rd[1] / rd_len;
        out_dir[2] = rd[2] / rd_len;
    } else {
        out_dir[0] = fwd[0];
        out_dir[1] = fwd[1];
        out_dir[2] = fwd[2];
    }
}

bool khr_ray_intersect_sphere(const float ro[3], const float rd[3],
                              const float center[3], float radius, float* out_t) {
    if (ro == nullptr || rd == nullptr || center == nullptr || radius <= 0.0f) {
        return false;
    }
    float m[3] = {
        ro[0] - center[0],
        ro[1] - center[1],
        ro[2] - center[2],
    };
    float b = m[0] * rd[0] + m[1] * rd[1] + m[2] * rd[2];
    float c = (m[0] * m[0] + m[1] * m[1] + m[2] * m[2]) - radius * radius;

    if (c > 0.0f && b > 0.0f) {
        return false;
    }
    float discr = b * b - c;
    if (discr < 0.0f) {
        return false;
    }
    float sqrt_d = sqrtf(discr);
    float t = -b - sqrt_d;
    if (t < 0.0f) {
        t = -b + sqrt_d;
    }
    if (t < 0.0f) {
        return false;
    }
    if (out_t != nullptr) {
        *out_t = t;
    }
    return true;
}

bool khr_ray_intersect_aabb(const float ro[3], const float rd[3],
                            const float min_p[3], const float max_p[3], float* out_t) {
    if (ro == nullptr || rd == nullptr || min_p == nullptr || max_p == nullptr) {
        return false;
    }
    float t_min = -1e30f;
    float t_max = 1e30f;

    for (int i = 0; i < 3; i++) {
        if (fabsf(rd[i]) > 1e-7f) {
            float inv_d = 1.0f / rd[i];
            float t1 = (min_p[i] - ro[i]) * inv_d;
            float t2 = (max_p[i] - ro[i]) * inv_d;
            float near = (t1 < t2) ? t1 : t2;
            float far  = (t1 > t2) ? t1 : t2;
            if (near > t_min) t_min = near;
            if (far < t_max)  t_max = far;
            if (t_min > t_max) return false;
        } else {
            if (ro[i] < min_p[i] || ro[i] > max_p[i]) {
                return false;
            }
        }
    }
    if (t_max < 0.0f) {
        return false;
    }
    float t = (t_min >= 0.0f) ? t_min : t_max;
    if (t < 0.0f) {
        return false;
    }
    if (out_t != nullptr) {
        *out_t = t;
    }
    return true;
}

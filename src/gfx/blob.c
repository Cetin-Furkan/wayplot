#include "khoros/gfx/blob.h"

#include <string.h>
#include <math.h>

#ifdef _STDBOOL_H
#error "<stdbool.h> is strictly banned. In C23, bool, true, and false are native language keywords."
#endif

#ifdef _STDALIGN_H
#error "<stdalign.h> is strictly banned. In C23, alignas and alignof are native language keywords."
#endif

size_t khr_blob_write(void* dst, size_t cap, const float* xyz, uint32_t nv,
                      const uint32_t* idx, uint32_t ni) {
    if (dst == nullptr || xyz == nullptr || idx == nullptr ||
        nv == 0 || ni < 3U || (ni % 3U) != 0U) {
        return 0;
    }
    const size_t need = khr_blob_bytes(nv, ni);
    if (cap < need) {
        return 0;
    }
    uint8_t* b = (uint8_t*)dst;
    memset(b, 0, need);
    khr_blob_t* h = (khr_blob_t*)b;
    float* verts = (float*)(b + sizeof(khr_blob_t));
    uint32_t* out_i = (uint32_t*)(b + sizeof(khr_blob_t) + (size_t)nv * 12U);
    h->magic = KHR_BLOB_MAGIC;
    h->version = KHR_BLOB_VERSION;
    h->vert_count = nv;
    h->index_count = ni;
    h->verts_off = (int32_t)((uint8_t*)verts - (uint8_t*)&h->verts_off);
    h->indices_off = (int32_t)((uint8_t*)out_i - (uint8_t*)&h->indices_off);
    memcpy(verts, xyz, (size_t)nv * 12U);
    memcpy(out_i, idx, (size_t)ni * 4U);
    return need;
}

size_t khr_blob_write_box(void* dst, size_t cap) {
    const float s = 0.45f;
    const float xyz[24] = {
        -s, -s, -s,   s, -s, -s,   s,  s, -s,  -s,  s, -s,
        -s, -s,  s,   s, -s,  s,   s,  s,  s,  -s,  s,  s,
    };
    /* Outward CCW. The previous table mixed windings; backface cull
     * then punched triangular holes in the faces. */
    const uint32_t faces[36] = {
        0, 2, 1,  0, 3, 2,  /* -Z */
        4, 5, 6,  4, 6, 7,  /* +Z */
        0, 1, 5,  0, 5, 4,  /* -Y */
        3, 7, 6,  3, 6, 2,  /* +Y */
        0, 4, 7,  0, 7, 3,  /* -X */
        1, 2, 6,  1, 6, 5,  /* +X */
    };
    return khr_blob_write(dst, cap, xyz, 8, faces, 36);
}

static void khr_cam_rebuild_r(khr_cam_t* cam) {
    float cy = cosf(cam->yaw);
    float sy = sinf(cam->yaw);
    float cx = cosf(cam->pitch);
    float sx = sinf(cam->pitch);
    cam->r0[0] = cy;
    cam->r0[1] = 0.0f;
    cam->r0[2] = -sy;
    cam->r1[0] = sy * sx;
    cam->r1[1] = cx;
    cam->r1[2] = cy * sx;
    cam->r2[0] = sy * cx;
    cam->r2[1] = -sx;
    cam->r2[2] = cy * cx;
}

void khr_cam_frame(khr_cam_t* cam, const float* xyz, uint32_t nv, bool reset_orient) {
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
                if (p[a] < mn[a]) {
                    mn[a] = p[a];
                }
                if (p[a] > mx[a]) {
                    mx[a] = p[a];
                }
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
        cam->radius = diag * 0.5f;
        if (cam->radius < 1.0e-4f) {
            cam->radius = 1.0e-4f;
        }
    }
    khr_cam_rebuild_r(cam);
}

void khr_cam_orbit(khr_cam_t* cam, float dyaw, float dpitch) {
    if (cam == nullptr) {
        return;
    }
    cam->yaw += dyaw;
    cam->pitch += dpitch;
    const float lim = 1.52f;
    if (cam->pitch > lim) {
        cam->pitch = lim;
    }
    if (cam->pitch < -lim) {
        cam->pitch = -lim;
    }
    khr_cam_rebuild_r(cam);
}

void khr_cam_pan(khr_cam_t* cam, float dx, float dy) {
    if (cam == nullptr) {
        return;
    }
    khr_cam_rebuild_r(cam);
    float k = cam->radius * 0.0025f;
    /* Screen +x is view +x; clip.y is flipped so screen +y (down) is view -y. */
    cam->target[0] -= (cam->r0[0] * dx - cam->r1[0] * dy) * k;
    cam->target[1] -= (cam->r0[1] * dx - cam->r1[1] * dy) * k;
    cam->target[2] -= (cam->r0[2] * dx - cam->r1[2] * dy) * k;
}

void khr_cam_zoom(khr_cam_t* cam, float ticks) {
    if (cam == nullptr || ticks == 0.0f) {
        return;
    }
    float f = 1.0f - ticks * 0.08f;
    if (f < 0.2f) {
        f = 0.2f;
    }
    if (f > 1.8f) {
        f = 1.8f;
    }
    cam->radius *= f;
    if (cam->radius < 1.0e-4f) {
        cam->radius = 1.0e-4f;
    }
    if (cam->radius > 1.0e6f) {
        cam->radius = 1.0e6f;
    }
}

void khr_cam_snap_axis(khr_cam_t* cam, int axis) {
    if (cam == nullptr || axis == 0) {
        return;
    }
    /* +N = look from that axis (the +N face toward the camera). */
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
    khr_cam_rebuild_r(cam);
}

int khr_cam_pick_axis(const khr_cam_t* cam, float nx, float ny) {
    if (cam == nullptr) {
        return 0;
    }
    float r2 = nx * nx + ny * ny;
    if (r2 < 0.12f) {
        float ax = cam->r0[2];
        float ay = cam->r1[2];
        float az = cam->r2[2];
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
    const float k = 0.78f;
    const float tips[6][3] = {
        {  1.0f,  cam->r0[0] * k, -cam->r0[1] * k },
        { -1.0f, -cam->r0[0] * k,  cam->r0[1] * k },
        {  2.0f,  cam->r1[0] * k, -cam->r1[1] * k },
        { -2.0f, -cam->r1[0] * k,  cam->r1[1] * k },
        {  3.0f,  cam->r2[0] * k, -cam->r2[1] * k },
        { -3.0f, -cam->r2[0] * k,  cam->r2[1] * k },
    };
    int best = 0;
    float best_d = 0.42f * 0.42f;
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

void khr_gizmo_arm_apply(khr_mesh_push_t* push, const float* r0, const float* r1,
                         const float* r2, int axis, uint32_t rgb) {
    if (push == nullptr || r0 == nullptr || r1 == nullptr || r2 == nullptr) {
        return;
    }
    float a0[3];
    float a1[3];
    float a2[3];
    if (axis == 1) {
        a0[0] = r1[0]; a0[1] = r1[1]; a0[2] = r1[2];
        a1[0] = -r0[0]; a1[1] = -r0[1]; a1[2] = -r0[2];
        a2[0] = r2[0]; a2[1] = r2[1]; a2[2] = r2[2];
    } else if (axis == 2) {
        a0[0] = r2[0]; a0[1] = r2[1]; a0[2] = r2[2];
        a1[0] = r1[0]; a1[1] = r1[1]; a1[2] = r1[2];
        a2[0] = -r0[0]; a2[1] = -r0[1]; a2[2] = -r0[2];
    } else {
        a0[0] = r0[0]; a0[1] = r0[1]; a0[2] = r0[2];
        a1[0] = r1[0]; a1[1] = r1[1]; a1[2] = r1[2];
        a2[0] = r2[0]; a2[1] = r2[1]; a2[2] = r2[2];
    }
    const float s = 0.82f;
    push->mvp_c0[0] = a0[0] * s;
    push->mvp_c0[1] = a0[1] * s;
    push->mvp_c0[2] = a0[2] * s * 0.5f;
    push->mvp_c0[3] = 0.0f;
    push->mvp_c1[0] = a1[0] * s;
    push->mvp_c1[1] = a1[1] * s;
    push->mvp_c1[2] = a1[2] * s * 0.5f;
    push->mvp_c1[3] = 0.0f;
    push->mvp_c2[0] = a2[0] * s;
    push->mvp_c2[1] = a2[1] * s;
    push->mvp_c2[2] = a2[2] * s * 0.5f;
    push->mvp_c2[3] = 0.0f;
    push->mvp_c3[0] = 0.0f;
    push->mvp_c3[1] = 0.0f;
    push->mvp_c3[2] = 0.5f;
    push->mvp_c3[3] = 1.0f;
    push->light_dir[0] = 0.35f;
    push->light_dir[1] = 0.55f;
    push->light_dir[2] = 1.00f;
    push->light_dir[3] = 0.0f;
    push->pad0 = rgb;
}

void khr_mesh_cam_apply(khr_mesh_push_t* push, const khr_cam_t* cam) {
    if (push == nullptr || cam == nullptr) {
        return;
    }
    float s = 0.78f / cam->radius;
    float c[3] = { cam->target[0], cam->target[1], cam->target[2] };
    const float* r0 = cam->r0;
    const float* r1 = cam->r1;
    const float* r2 = cam->r2;
    float rcx = r0[0] * c[0] + r1[0] * c[1] + r2[0] * c[2];
    float rcy = r0[1] * c[0] + r1[1] * c[1] + r2[1] * c[2];
    float rcz = r0[2] * c[0] + r1[2] * c[1] + r2[2] * c[2];
    float tx = -s * rcx;
    float ty = -s * rcy;
    float tz = -s * rcz;
    push->mvp_c0[0] = r0[0] * s;
    push->mvp_c0[1] = r0[1] * s;
    push->mvp_c0[2] = r0[2] * s * 0.5f;
    push->mvp_c0[3] = 0.0f;
    push->mvp_c1[0] = r1[0] * s;
    push->mvp_c1[1] = r1[1] * s;
    push->mvp_c1[2] = r1[2] * s * 0.5f;
    push->mvp_c1[3] = 0.0f;
    push->mvp_c2[0] = r2[0] * s;
    push->mvp_c2[1] = r2[1] * s;
    push->mvp_c2[2] = r2[2] * s * 0.5f;
    push->mvp_c2[3] = 0.0f;
    push->mvp_c3[0] = tx;
    push->mvp_c3[1] = ty;
    push->mvp_c3[2] = tz * 0.5f + 0.5f;
    push->mvp_c3[3] = 1.0f;
    const float lx = 0.42f;
    const float ly = 0.58f;
    const float lz = 1.00f;
    push->light_dir[0] = r0[0] * lx + r0[1] * ly + r0[2] * lz;
    push->light_dir[1] = r1[0] * lx + r1[1] * ly + r1[2] * lz;
    push->light_dir[2] = r2[0] * lx + r2[1] * ly + r2[2] * lz;
    push->light_dir[3] = 0.0f;
}

void khr_mesh_fit_view(khr_mesh_push_t* push, const float* xyz, uint32_t nv) {
    khr_cam_t cam = {};
    khr_cam_frame(&cam, xyz, nv, true);
    khr_mesh_cam_apply(push, &cam);
}

bool khr_mesh_setup(const void* base, size_t cap, VkDeviceAddress gpu_base,
                    khr_mesh_push_t* push) {
    khr_blob_view_t v = {};
    if (!khr_mesh_bind_blob(base, cap, gpu_base, push) ||
        !khr_blob_parse(base, cap, &v)) {
        return false;
    }
    khr_mesh_fit_view(push, v.verts, v.vert_count);
    return true;
}

size_t khr_blob_write_gizmo_arm(void* dst, size_t cap) {
    const float xyz[24] = {
        0.08f, -0.09f, -0.09f,  0.94f, -0.09f, -0.09f,
        0.94f,  0.09f, -0.09f,  0.08f,  0.09f, -0.09f,
        0.08f, -0.09f,  0.09f,  0.94f, -0.09f,  0.09f,
        0.94f,  0.09f,  0.09f,  0.08f,  0.09f,  0.09f,
    };
    const uint32_t faces[36] = {
        0, 2, 1,  0, 3, 2,
        4, 5, 6,  4, 6, 7,
        0, 1, 5,  0, 5, 4,
        3, 7, 6,  3, 6, 2,
        0, 4, 7,  0, 7, 3,
        1, 2, 6,  1, 6, 5,
    };
    return khr_blob_write(dst, cap, xyz, 8, faces, 36);
}

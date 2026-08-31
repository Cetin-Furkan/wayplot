#include "helper/math3d.h"

#include <errno.h>
#include <math.h>
#include <string.h>

#define WP_AABB_BIG 1e30f

void wp_aabb_reset(struct wp_aabb *b)
{
    int i;

    if (!b)
        return;
    for (i = 0; i < 3; i++) {
        b->min[i] = WP_AABB_BIG;
        b->max[i] = -WP_AABB_BIG;
    }
}

void wp_aabb_add(struct wp_aabb *b, float x, float y, float z)
{
    if (!b)
        return;
    if (x < b->min[0])
        b->min[0] = x;
    if (y < b->min[1])
        b->min[1] = y;
    if (z < b->min[2])
        b->min[2] = z;
    if (x > b->max[0])
        b->max[0] = x;
    if (y > b->max[1])
        b->max[1] = y;
    if (z > b->max[2])
        b->max[2] = z;
}

int wp_aabb_ok(const struct wp_aabb *b)
{
    int i;

    if (!b)
        return 0;
    for (i = 0; i < 3; i++) {
        if (!(b->min[i] <= b->max[i]))
            return 0;
        if (!isfinite(b->min[i]) || !isfinite(b->max[i]))
            return 0;
    }
    return 1;
}

int wp_aabb_union(struct wp_aabb *o, const struct wp_aabb *a, const struct wp_aabb *b)
{
    struct wp_aabb t;
    int i;
    int oa, ob;

    if (!o || !a || !b)
        return -EINVAL;
    oa = wp_aabb_ok(a);
    ob = wp_aabb_ok(b);
    if (!oa && !ob)
        return -EINVAL;
    if (!oa) {
        *o = *b;
        return 0;
    }
    if (!ob) {
        *o = *a;
        return 0;
    }
    for (i = 0; i < 3; i++) {
        t.min[i] = a->min[i] < b->min[i] ? a->min[i] : b->min[i];
        t.max[i] = a->max[i] > b->max[i] ? a->max[i] : b->max[i];
    }
    *o = t;
    return 0;
}

void wp_aabb_center(const struct wp_aabb *b, float c[3])
{
    int i;

    if (!c)
        return;
    if (!wp_aabb_ok(b)) {
        c[0] = c[1] = c[2] = 0.0f;
        return;
    }
    for (i = 0; i < 3; i++)
        c[i] = 0.5f * (b->min[i] + b->max[i]);
}

float wp_aabb_radius(const struct wp_aabb *b)
{
    float e[3], r;
    int i;

    if (!wp_aabb_ok(b))
        return 0.0f;
    for (i = 0; i < 3; i++)
        e[i] = 0.5f * (b->max[i] - b->min[i]);
    r = sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
    return r;
}

void wp_vec3_normalize(float v[3])
{
    float s = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (s <= 1e-12f)
        return;
    v[0] /= s;
    v[1] /= s;
    v[2] /= s;
}

void wp_vec3_cross(float o[3], const float a[3], const float b[3])
{
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}

float wp_vec3_dot(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void wp_mat4_identity(float m[16])
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void wp_mat4_mul(float r[16], const float a[16], const float b[16])
{
    float t[16];
    int i, j, k;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            float s = 0.0f;
            for (k = 0; k < 4; k++)
                s += a[k * 4 + i] * b[j * 4 + k];
            t[j * 4 + i] = s;
        }
    }
    memcpy(r, t, sizeof(t));
}

void wp_mat4_perspective_vk(float m[16], float fovy_rad, float aspect, float znear, float zfar)
{
    float f = 1.0f / tanf(fovy_rad * 0.5f);
    memset(m, 0, 16 * sizeof(float));
    m[0] = f / aspect;
    m[5] = -f; /* Vulkan NDC y is down */
    m[10] = zfar / (znear - zfar);
    m[11] = -1.0f;
    m[14] = (znear * zfar) / (znear - zfar);
}

void wp_mat4_ortho_vk(float m[16], float half_h, float aspect, float znear, float zfar)
{
    float half_w;

    if (half_h < 1e-6f)
        half_h = 1e-6f;
    if (aspect < 1e-6f)
        aspect = 1.0f;
    half_w = half_h * aspect;
    memset(m, 0, 16 * sizeof(float));
    m[0] = 1.0f / half_w;
    m[5] = -1.0f / half_h; /* Vulkan NDC y is down; view +Y is screen up */
    m[10] = 1.0f / (znear - zfar);
    m[14] = znear / (znear - zfar);
    m[15] = 1.0f;
}

void wp_mat4_lookat_rh(float m[16], const float eye[3], const float center[3], const float up[3])
{
    float f[3] = { center[0] - eye[0], center[1] - eye[1], center[2] - eye[2] };
    float s[3], u[3], upn[3] = { up[0], up[1], up[2] };

    wp_vec3_normalize(f);
    wp_vec3_normalize(upn);
    wp_vec3_cross(s, f, upn);
    wp_vec3_normalize(s);
    wp_vec3_cross(u, s, f);

    memset(m, 0, 16 * sizeof(float));
    m[0] = s[0];
    m[4] = s[1];
    m[8] = s[2];
    m[1] = u[0];
    m[5] = u[1];
    m[9] = u[2];
    m[2] = -f[0];
    m[6] = -f[1];
    m[10] = -f[2];
    m[12] = -wp_vec3_dot(s, eye);
    m[13] = -wp_vec3_dot(u, eye);
    m[14] = wp_vec3_dot(f, eye);
    m[15] = 1.0f;
}

void wp_mat4_mul_vec4(float out[4], const float m[16], const float v[4])
{
    float t[4];
    int r, c;
    for (r = 0; r < 4; r++) {
        float s = 0.0f;
        for (c = 0; c < 4; c++)
            s += m[c * 4 + r] * v[c];
        t[r] = s;
    }
    memcpy(out, t, sizeof(t));
}

void wp_clip_to_ndc(float ndc[3], const float clip[4])
{
    ndc[0] = clip[0] / clip[3];
    ndc[1] = clip[1] / clip[3];
    ndc[2] = clip[2] / clip[3];
}

float wp_ndc_signed_area(const float a[3], const float b[3], const float c[3])
{
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
}

float wp_triangle_ndc_area(const float clip_m[16],
                           const float a[3], const float b[3], const float c[3])
{
    float pa[4] = { a[0], a[1], a[2], 1.0f };
    float pb[4] = { b[0], b[1], b[2], 1.0f };
    float pc[4] = { c[0], c[1], c[2], 1.0f };
    float ca[4], cb[4], cc[4], na[3], nb[3], nc[3];

    wp_mat4_mul_vec4(ca, clip_m, pa);
    wp_mat4_mul_vec4(cb, clip_m, pb);
    wp_mat4_mul_vec4(cc, clip_m, pc);
    if (ca[3] <= 1e-8f || cb[3] <= 1e-8f || cc[3] <= 1e-8f)
        return 0.0f;
    wp_clip_to_ndc(na, ca);
    wp_clip_to_ndc(nb, cb);
    wp_clip_to_ndc(nc, cc);
    return wp_ndc_signed_area(na, nb, nc);
}

bool wp_triangle_front_facing(const float clip_m[16],
                              const float a[3], const float b[3], const float c[3])
{
    float area = wp_triangle_ndc_area(clip_m, a, b, c);
    if (fabsf(area) < 1e-8f)
        return false;
    return (area < 0.0f) == (WP_FRONT_NDC_AREA_SIGN < 0);
}

void wp_mat4_ortho_pixel(float m[16], float width, float height)
{
    if (width < 1e-6f)
        width = 1.0f;
    if (height < 1e-6f)
        height = 1.0f;
    memset(m, 0, 16 * sizeof(float));
    m[0] = 2.0f / width;
    m[5] = 2.0f / height;
    m[10] = 1.0f;
    m[12] = -1.0f;
    m[13] = -1.0f;
    m[15] = 1.0f;
}

void wp_mat4_rotate(float m[16], float rad, float ax, float ay, float az)
{
    float axis[3] = { ax, ay, az };
    float c, s, ic, x, y, z;
    wp_vec3_normalize(axis);
    x = axis[0];
    y = axis[1];
    z = axis[2];
    c = cosf(rad);
    s = sinf(rad);
    ic = 1.0f - c;
    wp_mat4_identity(m);
    m[0] = c + x * x * ic;
    m[1] = y * x * ic + z * s;
    m[2] = z * x * ic - y * s;
    m[4] = x * y * ic - z * s;
    m[5] = c + y * y * ic;
    m[6] = z * y * ic + x * s;
    m[8] = x * z * ic + y * s;
    m[9] = y * z * ic - x * s;
    m[10] = c + z * z * ic;
}

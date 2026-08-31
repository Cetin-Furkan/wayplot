#include "renderer/camera.h"

#include "helper/math3d.h"

#include <math.h>
#include <string.h>

void wp_camera_default(struct wp_camera *c)
{
    /* Corner view so a unit cube shows three faces, not a head-on square. */
    memset(c, 0, sizeof(*c));
    c->eye[0] = 1.6f;
    c->eye[1] = 1.2f;
    c->eye[2] = 2.0f;
    c->up[1] = 1.0f;
    c->fovy_rad = 1.04719755f; /* 60 deg */
    c->znear = 0.1f;
    c->zfar = 20.0f;
}

bool wp_camera_front_clockwise(void)
{
    /* Vulkan a = -raw_shoelace. Front faces have negative raw shoelace
     * (WP_FRONT_NDC_AREA_SIGN < 0) and therefore positive Vulkan a, which
     * is FRONT_FACE_COUNTER_CLOCKWISE. Mapping the raw sign onto CLOCKWISE
     * is how the cube was drawn inside-out. */
    return WP_FRONT_NDC_AREA_SIGN > 0;
}

void wp_camera_view(const struct wp_camera *c, float view[16])
{
    wp_mat4_lookat_rh(view, c->eye, c->center, c->up);
}

void wp_camera_proj(const struct wp_camera *c, float aspect, float proj[16])
{
    float d[3], dist, half;

    if (aspect < 1e-6f)
        aspect = 1.0f;
    if (!c->ortho) {
        wp_mat4_perspective_vk(proj, c->fovy_rad, aspect, c->znear, c->zfar);
        return;
    }
    d[0] = c->center[0] - c->eye[0];
    d[1] = c->center[1] - c->eye[1];
    d[2] = c->center[2] - c->eye[2];
    dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (dist < 1e-4f)
        dist = 1.0f;
    half = dist * tanf(c->fovy_rad * 0.5f);
    if (half < 1e-6f)
        half = 1e-6f;
    wp_mat4_ortho_vk(proj, half, aspect, c->znear, c->zfar);
}

void wp_camera_vp(const struct wp_camera *c, float aspect, float vp[16])
{
    float view[16], proj[16];
    wp_camera_view(c, view);
    wp_camera_proj(c, aspect, proj);
    wp_mat4_mul(vp, proj, view);
}

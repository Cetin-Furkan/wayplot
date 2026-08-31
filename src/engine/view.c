#define _GNU_SOURCE
#include "engine/view.h"

#include "helper/math3d.h"

#include <errno.h>
#include <math.h>
#include <string.h>

static void sync_cam(struct wp_view *v)
{
    float cp;

    v->cam.center[0] = v->center[0];
    v->cam.center[1] = v->center[1];
    v->cam.center[2] = v->center[2];
    if (v->plan) {
        /* Look −Y. up in XZ so look-at does not degenerate (world +Y ∥ view). */
        v->cam.eye[0] = v->center[0];
        v->cam.eye[1] = v->center[1] + v->dist;
        v->cam.eye[2] = v->center[2];
        v->cam.up[0] = sinf(v->yaw);
        v->cam.up[1] = 0.0f;
        v->cam.up[2] = cosf(v->yaw);
        v->cam.ortho = 1;
        return;
    }
    cp = cosf(v->pitch);
    v->cam.eye[0] = v->center[0] + v->dist * sinf(v->yaw) * cp;
    v->cam.eye[1] = v->center[1] + v->dist * sinf(v->pitch);
    v->cam.eye[2] = v->center[2] + v->dist * cosf(v->yaw) * cp;
    v->cam.up[0] = 0.0f;
    v->cam.up[1] = 1.0f;
    v->cam.up[2] = 0.0f;
    v->cam.ortho = 0;
}

static void capture(struct wp_view *v)
{
    float o[3], d;

    v->center[0] = v->cam.center[0];
    v->center[1] = v->cam.center[1];
    v->center[2] = v->cam.center[2];
    o[0] = v->cam.eye[0] - v->center[0];
    o[1] = v->cam.eye[1] - v->center[1];
    o[2] = v->cam.eye[2] - v->center[2];
    d = sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
    if (d < 1e-4f)
        d = 1.0f;
    v->dist = d;
    v->yaw = atan2f(o[0], o[2]);
    {
        float s = o[1] / d;
        if (s > 1.0f)
            s = 1.0f;
        if (s < -1.0f)
            s = -1.0f;
        v->pitch = asinf(s);
    }
    if (v->pitch > WP_VIEW_PITCH_LIM)
        v->pitch = WP_VIEW_PITCH_LIM;
    if (v->pitch < -WP_VIEW_PITCH_LIM)
        v->pitch = -WP_VIEW_PITCH_LIM;
    sync_cam(v);
}

void wp_view_init(struct wp_view *v)
{
    if (!v)
        return;
    memset(v, 0, sizeof(*v));
    wp_camera_default(&v->cam);
    v->rect = (struct wp_rect){ 0, 0, 1280, 720 };
    v->layers = ~0u;
    v->dist_min = WP_VIEW_DIST_MIN;
    v->dist_max = WP_VIEW_DIST_MAX;
    capture(v);
}

int wp_view_fit(struct wp_view *v, const struct wp_aabb *box)
{
    float radius, aspect, half, tan_half, r;
    float c[3];

    if (!v || !wp_aabb_ok(box))
        return -EINVAL;
    wp_aabb_center(box, c);
    v->center[0] = c[0];
    v->center[1] = c[1];
    v->center[2] = c[2];
    radius = wp_aabb_radius(box);
    if (radius < 1e-4f)
        radius = 0.5f;
    aspect = 1.0f;
    if (v->rect.h > 1e-4f && v->rect.w > 1e-4f)
        aspect = v->rect.w / v->rect.h;
    r = radius * WP_VIEW_FIT_PAD;
    if (aspect < 1.0f)
        r /= aspect;
    tan_half = tanf(v->cam.fovy_rad * 0.5f);
    if (tan_half < 1e-6f)
        tan_half = 1e-6f;
    half = r;
    v->dist = half / tan_half;
    v->dist_min = v->dist * 0.05f;
    if (v->dist_min < 1e-3f)
        v->dist_min = 1e-3f;
    v->dist_max = v->dist * 20.0f;
    v->cam.znear = v->dist * 0.02f;
    if (v->cam.znear > radius * 0.25f)
        v->cam.znear = radius * 0.25f;
    if (v->cam.znear < 1e-3f)
        v->cam.znear = 1e-3f;
    v->cam.zfar = v->dist + radius * 8.0f;
    if (v->cam.zfar < v->cam.znear * 10.0f)
        v->cam.zfar = v->cam.znear * 10.0f;
    sync_cam(v);
    return 0;
}

int wp_view_mesh_on(const struct wp_view *v, uint32_t mesh_i)
{
    if (!v || mesh_i >= 32u)
        return 0;
    return (int)((v->layers >> mesh_i) & 1u);
}

void wp_view_plan(struct wp_view *v)
{
    if (!v)
        return;
    v->plan = 1;
    v->yaw = 0.0f;
    v->pitch = WP_VIEW_PITCH_LIM;
    if (v->dist < WP_VIEW_DIST_MIN)
        v->dist = 3.0f;
    sync_cam(v);
}

void wp_view_orbit(struct wp_view *v, float dx_px, float dy_px)
{
    if (!v)
        return;
    if (dx_px == 0.0f && dy_px == 0.0f)
        return;
    if (v->plan) {
        /* Yaw rotates the map. Pitch does not leave plan — the 3D pane tilts. */
        v->yaw += dx_px * WP_VIEW_ORBIT_RAD;
        sync_cam(v);
        return;
    }
    v->yaw += dx_px * WP_VIEW_ORBIT_RAD;
    v->pitch -= dy_px * WP_VIEW_ORBIT_RAD; /* logical Y-down: drag up looks up */
    if (v->pitch > WP_VIEW_PITCH_LIM)
        v->pitch = WP_VIEW_PITCH_LIM;
    if (v->pitch < -WP_VIEW_PITCH_LIM)
        v->pitch = -WP_VIEW_PITCH_LIM;
    sync_cam(v);
}

void wp_view_pan(struct wp_view *v, float dx_px, float dy_px)
{
    float fwd[3], right[3], up[3], s, n2;

    if (!v)
        return;
    if (dx_px == 0.0f && dy_px == 0.0f)
        return;
    fwd[0] = v->center[0] - v->cam.eye[0];
    fwd[1] = v->center[1] - v->cam.eye[1];
    fwd[2] = v->center[2] - v->cam.eye[2];
    wp_vec3_normalize(fwd);
    /* cam.up, not world +Y: a plan view has forward ∥ world +Y. */
    wp_vec3_cross(right, fwd, v->cam.up);
    n2 = right[0] * right[0] + right[1] * right[1] + right[2] * right[2];
    if (n2 < 1e-12f)
        return;
    wp_vec3_normalize(right);
    wp_vec3_cross(up, right, fwd);
    wp_vec3_normalize(up);
    s = v->dist * WP_VIEW_PAN;
    /* Content follows the pointer: drag right → camera left. Y-down. */
    v->center[0] += (-right[0] * dx_px + up[0] * dy_px) * s;
    v->center[1] += (-right[1] * dx_px + up[1] * dy_px) * s;
    v->center[2] += (-right[2] * dx_px + up[2] * dy_px) * s;
    sync_cam(v);
}

void wp_view_dolly(struct wp_view *v, int32_t axis_fixed)
{
    float f;

    if (!v || axis_fixed == 0)
        return;
    f = expf(((float)axis_fixed / 256.0f) * WP_VIEW_DOLLY);
    v->dist *= f;
    if (v->dist < v->dist_min)
        v->dist = v->dist_min;
    if (v->dist > v->dist_max)
        v->dist = v->dist_max;
    sync_cam(v);
}

int wp_view_pixels(const struct wp_view *v, int32_t scale, uint32_t fb_w, uint32_t fb_h,
                   int32_t *x, int32_t *y, uint32_t *w, uint32_t *h)
{
    int32_t s, x0, y0, x1, y1;

    if (!v || !wp_rect_ok(&v->rect) || !x || !y || !w || !h)
        return -EINVAL;
    if (fb_w == 0 || fb_h == 0)
        return -EINVAL;
    s = scale > 0 ? scale : 1;
    x0 = (int32_t)(v->rect.x * (float)s);
    y0 = (int32_t)(v->rect.y * (float)s);
    x1 = (int32_t)((v->rect.x + v->rect.w) * (float)s);
    y1 = (int32_t)((v->rect.y + v->rect.h) * (float)s);
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > (int32_t)fb_w)
        x1 = (int32_t)fb_w;
    if (y1 > (int32_t)fb_h)
        y1 = (int32_t)fb_h;
    if (x1 <= x0 || y1 <= y0)
        return -EINVAL;
    *x = x0;
    *y = y0;
    *w = (uint32_t)(x1 - x0);
    *h = (uint32_t)(y1 - y0);
    return 0;
}

int wp_view_bind(const struct wp_view *v, VkCommandBuffer cmd, int32_t scale, uint32_t fb_w,
                 uint32_t fb_h)
{
    int32_t x, y;
    uint32_t w, h;
    VkViewport vp;
    VkRect2D sc;
    int ret;

    if (!cmd)
        return -EINVAL;
    ret = wp_view_pixels(v, scale, fb_w, fb_h, &x, &y, &w, &h);
    if (ret < 0)
        return ret;
    vp = (VkViewport){
        .x = (float)x,
        .y = (float)y,
        .width = (float)w,
        .height = (float)h,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    sc = (VkRect2D){
        .offset = { x, y },
        .extent = { w, h },
    };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    return 0;
}

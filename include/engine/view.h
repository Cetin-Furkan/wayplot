#ifndef ENGINE_VIEW_H
#define ENGINE_VIEW_H

#include "renderer/camera.h"
#include "renderer/card.h"

#include <stdint.h>
#include <vulkan/vulkan.h>

/*
 * One pane: a logical rect + a camera. Scissor/viewport inside the opaque
 * pass, not a third BeginRendering. The view owns the camera.
 * See docs/VIEW.md, docs/PLAN.md, docs/LAYERS.md, docs/ORTHO.md, and docs/FIT.md.
 */

#define WP_VIEW_ORBIT_RAD 0.005f
#define WP_VIEW_PITCH_LIM 1.55334306f /* ~89 deg; turntable, not a free gimbal */
#define WP_VIEW_PAN 0.0015f
#define WP_VIEW_DOLLY 0.08f
#define WP_VIEW_DIST_MIN 0.15f
#define WP_VIEW_DIST_MAX 18.0f
#define WP_VIEW_FIT_PAD 1.35f

struct wp_view {
    struct wp_rect rect; /* logical surface pixels, Y down */
    struct wp_camera cam;
    float yaw, pitch, dist;
    float dist_min, dist_max; /* dolly clamp; fit replaces the unit-cube 18 cap */
    float center[3];
    int plan; /* 1: eye above, look −Y, ortho; yaw-only orbit. See docs/PLAN.md. */
    uint32_t layers; /* bit i = doc mesh i. Init all-on. See docs/LAYERS.md. */
};

void wp_view_init(struct wp_view *v);
void wp_view_plan(struct wp_view *v);
[[nodiscard]] int wp_view_fit(struct wp_view *v, const struct wp_aabb *box);
int wp_view_mesh_on(const struct wp_view *v, uint32_t mesh_i);
void wp_view_orbit(struct wp_view *v, float dx_px, float dy_px);
void wp_view_pan(struct wp_view *v, float dx_px, float dy_px);
/* axis_fixed is wl_fixed (256 = 1.0). Positive vertical = zoom out. */
void wp_view_dolly(struct wp_view *v, int32_t axis_fixed);

/* Buffer-pixel box for this view. scale is integer buffer scale. */
[[nodiscard]] int wp_view_pixels(const struct wp_view *v, int32_t scale, uint32_t fb_w,
                                 uint32_t fb_h, int32_t *x, int32_t *y, uint32_t *w, uint32_t *h);

/* Viewport + scissor = the pane. Pass begin still cleared the full FB. */
[[nodiscard]] int wp_view_bind(const struct wp_view *v, VkCommandBuffer cmd, int32_t scale,
                               uint32_t fb_w, uint32_t fb_h);

#endif /* ENGINE_VIEW_H */

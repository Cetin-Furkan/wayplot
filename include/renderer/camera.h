#ifndef RENDERER_CAMERA_H
#define RENDERER_CAMERA_H

#include "helper/math3d.h"

#include <stdbool.h>

/*
 * Right-handed Y-up world. Projection is Vulkan 0-1 depth with Y flipped
 * (m[5] < 0) so world +Y is up on screen. Plan views set ortho (docs/ORTHO.md).
 *
 * Mesh winding is CCW from the outside. The Y-flip keeps that CCW on
 * screen. Vulkan's facing area has a leading minus (Y-down FB), so
 * positive Vulkan `a` is COUNTER_CLOCKWISE: wp_camera_front_clockwise()
 * is false and the pipeline is FRONT_FACE_COUNTER_CLOCKWISE + CULL_BACK.
 * A triangle is drawn only when the camera sees its outward side
 * (wp_triangle_front_facing). Do not reverse indices, disable culling,
 * or draw both sides.
 *
 * Locked by math3d_test, mesh_test, and raster_test (GPU pixels).
 */

struct wp_camera {
    float eye[3];
    float center[3];
    float up[3];
    float fovy_rad;
    float znear;
    float zfar;
    int ortho; /* 1: wp_mat4_ortho_vk. half-height = |eye-center| * tan(fovy/2). */
};

void wp_camera_default(struct wp_camera *c);
void wp_camera_view(const struct wp_camera *c, float view[16]);
void wp_camera_proj(const struct wp_camera *c, float aspect, float proj[16]);
void wp_camera_vp(const struct wp_camera *c, float aspect, float vp[16]);

/* True only if the pipeline must use VK_FRONT_FACE_CLOCKWISE. False
 * with the Y-flipped projection (Vulkan a = -shoelace → CCW). */
bool wp_camera_front_clockwise(void);

#endif /* RENDERER_CAMERA_H */

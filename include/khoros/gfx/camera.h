#ifndef KHOROS_GFX_CAMERA_H
#define KHOROS_GFX_CAMERA_H

#if __STDC_VERSION__ < 202311L
#error "Khoros requires pure ISO C23 (compile with -std=c23)"
#endif

#ifdef _STDBOOL_H
#error "<stdbool.h> is strictly banned. In C23, bool, true, and false are native language keywords."
#endif

#ifdef _STDALIGN_H
#error "<stdalign.h> is strictly banned. In C23, alignas and alignof are native language keywords."
#endif

#include "khoros/core/attributes.h"
#include "khoros/gfx/gpu_math.h"

/*
 * Decoupled 3D Camera System (Pillar B).
 * Supports both Spherical Orbiting and Quaternion Arcball Controllers.
 */
typedef struct {
    float eye[3];
    float target[3];
    float up[3];
    float fov_y;     /* Vertical Field of View (radians) */
    float aspect;    /* Viewport aspect ratio (width / height) */
    float z_near;    /* Near clipping plane distance */
    float z_far;     /* Far clipping plane distance */
    float radius;    /* Distance from eye to target */
    float yaw;       /* Orbit azimuth angle (radians) */
    float pitch;     /* Orbit elevation angle (radians) */
    float quat[4];   /* Orientation quaternion (x, y, z, w) for Arcball */
} khr_camera_t;

/* Initialize perspective camera with standard defaults */
void khr_camera_init(khr_camera_t* cam, float fov_y_rad, float aspect, float z_near);

/* Explicitly set camera eye, target, and up vectors */
void khr_camera_look_at(khr_camera_t* cam, const float eye[3], const float target[3], const float up[3]);

/* Spherical orbit rotation (modifies yaw and pitch) */
void khr_camera_orbit(khr_camera_t* cam, float dyaw, float dpitch);

/*
 * Quaternion Arcball virtual sphere rotation.
 * Takes normalized screen coordinates in [-1, 1] for drag start (p0) and drag current (p1).
 * Completely eliminates gimbal lock and pole flipping.
 */
void khr_camera_arcball(khr_camera_t* cam, float p0_x, float p0_y, float p1_x, float p1_y);

/* Pan camera in the view plane */
void khr_camera_pan(khr_camera_t* cam, float dx, float dy);

/* Zoom camera toward/away from target */
void khr_camera_zoom(khr_camera_t* cam, float delta);

/* Update viewport aspect ratio */
void khr_camera_set_aspect(khr_camera_t* cam, float aspect);

/* Fit camera view to an Axis-Aligned Bounding Box (AABB) */
void khr_camera_fit_aabb(khr_camera_t* cam, const float min_p[3], const float max_p[3]);

/* Feed camera parameters directly into GPU compute culling push constant */
void khr_camera_feed_cull_push(const khr_camera_t* cam, khr_cull_push_t* push);

/* Extract 3x3 orientation row vectors from camera for gizmo/view transforms */
void khr_camera_get_rotation_matrix(const khr_camera_t* cam, float r0[3], float r1[3], float r2[3]);

/* Snap camera to a cardinal axis (±1 = ±X, ±2 = ±Y, ±3 = ±Z) */
void khr_camera_snap_axis(khr_camera_t* cam, int axis);

/* Pick gimbal axis from normalized 2D coordinates [-1, 1] */
[[nodiscard]]
int khr_camera_pick_axis(const khr_camera_t* cam, float nx, float ny);

/* Frame a list of packed vertices in 3D space */
void khr_camera_frame_verts(khr_camera_t* cam, const float* xyz, uint32_t nv, bool reset_orient);

#endif /* KHOROS_GFX_CAMERA_H */

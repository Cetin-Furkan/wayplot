#ifndef HELPER_MATH3D_H
#define HELPER_MATH3D_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Column-major 4x4: m[col * 4 + row]. CPU mul is columns: out = c0*x + c1*y + c2*z + c3*w.
 * Shaders must multiply the same way. Do not put float4x4 in a UBO — Slang/std140
 * has marked those RowMajor even with -matrix-layout-column-major, which transposes
 * the projection and turns a cube into a dart.
 *
 * wp_mat4_perspective_vk stores m[5] = -f so world +Y is up on screen.
 * Mesh triangles are CCW from the outside. After the Y-flip they are CCW on
 * screen. Vulkan's facing area is
 *
 *   a = -1/2 * sum (x_i y_{i+1} - x_{i+1} y_i)   in Y-down framebuffer
 *
 * The leading minus makes positive `a` mean COUNTER_CLOCKWISE on screen.
 * So the pipeline is FRONT_FACE_COUNTER_CLOCKWISE + CULL_BACK: a triangle
 * is drawn iff the camera is in its outward half-space.
 *
 * Raw NDC shoelace (this file, no minus) is therefore NEGATIVE for a front
 * face. Do not map that to VK_FRONT_FACE_CLOCKWISE — that draws the insides
 * (a dart, then an inside-out cube). Do not reverse indices or disable cull.
 * Locked by test-math3d, test-renderer-mesh, and test-renderer-raster (GPU
 * pixel readback of a known-front face).
 */

enum {
    /* Sign of raw NDC shoelace (Y-down, no Vulkan minus) for a front face. */
    WP_FRONT_NDC_AREA_SIGN = -1,
};

struct wp_aabb {
    float min[3];
    float max[3];
};

void wp_aabb_reset(struct wp_aabb *b);
void wp_aabb_add(struct wp_aabb *b, float x, float y, float z);
int wp_aabb_ok(const struct wp_aabb *b);
[[nodiscard]] int wp_aabb_union(struct wp_aabb *o, const struct wp_aabb *a, const struct wp_aabb *b);
void wp_aabb_center(const struct wp_aabb *b, float c[3]);
float wp_aabb_radius(const struct wp_aabb *b);

void wp_vec3_normalize(float v[3]);
void wp_vec3_cross(float o[3], const float a[3], const float b[3]);
float wp_vec3_dot(const float a[3], const float b[3]);

void wp_mat4_identity(float m[16]);
void wp_mat4_mul(float r[16], const float a[16], const float b[16]);
void wp_mat4_mul_vec4(float out[4], const float m[16], const float v[4]);
void wp_mat4_perspective_vk(float m[16], float fovy_rad, float aspect, float znear, float zfar);
/* Parallel box, same Y-flip and Vulkan 0-1 depth as perspective. half_h is
 * view-space half height. See docs/ORTHO.md. */
void wp_mat4_ortho_vk(float m[16], float half_h, float aspect, float znear, float zfar);
void wp_mat4_lookat_rh(float m[16], const float eye[3], const float center[3], const float up[3]);
void wp_mat4_rotate(float m[16], float rad, float ax, float ay, float az);
/* Pixel (0,0) top-left, Y down → Vulkan NDC. Same map as shaders/text.slang. */
void wp_mat4_ortho_pixel(float m[16], float width, float height);
void wp_clip_to_ndc(float ndc[3], const float clip[4]);
float wp_ndc_signed_area(const float a[3], const float b[3], const float c[3]);

/* Project abc through clip_m (usually proj*view*model). 0 if any vertex is
 * behind the near plane (clip.w <= 0) or the triangle is degenerate. */
float wp_triangle_ndc_area(const float clip_m[16],
                           const float a[3], const float b[3], const float c[3]);

/* True when the triangle's NDC winding is the raster front face. */
bool wp_triangle_front_facing(const float clip_m[16],
                              const float a[3], const float b[3], const float c[3]);

#endif /* HELPER_MATH3D_H */

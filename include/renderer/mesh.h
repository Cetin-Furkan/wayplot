#ifndef RENDERER_MESH_H
#define RENDERER_MESH_H

#include "helper/math3d.h"
#include "vulkan/buffer.h"
#include "vulkan/device.h"

#include <stdint.h>

/* Lit-mesh vertex: world-space position, outward normal, linear RGB, UV.
 * Triangle winding is CCW when looking along the outward normal (outside).
 * Do not reverse indices to chase a raster dart — fix camera/frontFace.
 * uv 0,0 with the default 1×1 white albedo is vertex color only. */
struct wp_vn_vertex {
    float px, py, pz;
    float nx, ny, nz;
    float r, g, b;
    float u, v;
};

struct wp_mesh {
    struct wp_buffer vbo;
    struct wp_buffer ibo;
    uint32_t vertex_count;
    uint32_t index_count;
    struct wp_aabb aabb; /* world box of the uploaded verts. See docs/FIT.md. */
};

/* uint16 indices: vertex count fits in 0..65535. */
#define WP_MESH_MAX_V 65536u
#define WP_MESH_MAX_I (1u << 20)

/* Heap CPU mesh. File load and tests own this; GPU upload copies it. */
struct wp_mesh_cpu {
    struct wp_vn_vertex *v;
    uint16_t *idx;
    uint32_t nv;
    uint32_t ni;
};

void wp_mesh_cpu_free(struct wp_mesh_cpu *g);

void wp_cube_cpu(struct wp_vn_vertex *v, uint16_t *idx, uint32_t *nv, uint32_t *ni);
[[nodiscard]] int wp_aabb_from_vn(struct wp_aabb *b, const struct wp_vn_vertex *v, uint32_t n);
[[nodiscard]] int wp_mesh_upload(struct wp_device *d, struct wp_mesh *m,
                                 const struct wp_vn_vertex *v, uint32_t nv,
                                 const uint16_t *idx, uint32_t ni);
[[nodiscard]] int wp_mesh_cube(struct wp_device *d, struct wp_mesh *m);
void wp_mesh_destroy(struct wp_device *d, struct wp_mesh *m);

#endif /* RENDERER_MESH_H */

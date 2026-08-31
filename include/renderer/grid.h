#ifndef RENDERER_GRID_H
#define RENDERER_GRID_H

#include "helper/math3d.h"
#include "renderer/mesh.h"

#include <stdint.h>

/*
 * World XZ grid → wp_mesh_cpu. Thin quads, CCW from +Y, same lit pass.
 * Spacing from the scene AABB. See docs/GRID.md.
 */

#define WP_GRID_TARGET 12
#define WP_GRID_MAX_LINES 96
#define WP_GRID_MAJOR 10

struct wp_grid {
    float y;
    float x0, x1, z0, z1;
    float step;
    int ix0, ix1, iz0, iz1;
};

[[nodiscard]] int wp_grid_from_aabb(const struct wp_aabb *box, struct wp_grid *g);
[[nodiscard]] int wp_grid_tessellate(const struct wp_grid *g, struct wp_mesh_cpu *out);

#endif /* RENDERER_GRID_H */

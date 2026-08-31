#ifndef RENDERER_DEM_H
#define RENDERER_DEM_H

#include "renderer/mesh.h"

#include <stddef.h>
#include <stdint.h>

/*
 * Heightmap → wp_mesh_cpu. Grid in XZ, height on +Y. CCW from above.
 * PGM P5 (binary). Bad content is a negative errno. See docs/DEM.md.
 */

#define WP_DEM_MAX_SIDE 256
#define WP_DEM_AMP 0.5f

struct wp_dem {
    float *h; /* row-major, j * cols + i */
    uint32_t cols;
    uint32_t rows;
};

void wp_dem_free(struct wp_dem *d);

[[nodiscard]] int wp_dem_parse_pgm(const void *data, size_t len, struct wp_dem *out);
[[nodiscard]] int wp_dem_load(const char *path, struct wp_dem *out);
[[nodiscard]] int wp_dem_tessellate(const struct wp_dem *d, float amp, struct wp_mesh_cpu *out);

#endif /* RENDERER_DEM_H */

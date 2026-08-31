#ifndef RENDERER_PLOT_H
#define RENDERER_PLOT_H

#include "renderer/mesh.h"

#include <stddef.h>
#include <stdint.h>

/*
 * 1D series → wp_mesh_cpu. Ribbon in XZ, height on +Y, both windings
 * (CULL_BACK, not CULL_NONE). ASCII floats, '#' comments. See docs/PLOT.md.
 */

#define WP_PLOT_MAX 4096
#define WP_PLOT_AMP 0.5f
#define WP_PLOT_HALF_W 0.5f

struct wp_plot {
    float *y;
    uint32_t n;
};

void wp_plot_free(struct wp_plot *p);
[[nodiscard]] int wp_plot_resize(struct wp_plot *p, uint32_t n);

[[nodiscard]] int wp_plot_parse(const void *data, size_t len, struct wp_plot *out);
[[nodiscard]] int wp_plot_load(const char *path, struct wp_plot *out);
[[nodiscard]] int wp_plot_tessellate(const struct wp_plot *p, float amp, struct wp_mesh_cpu *out);

#endif /* RENDERER_PLOT_H */

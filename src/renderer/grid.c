#define _GNU_SOURCE
#include "renderer/grid.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define WP_GRID_MINOR_FRAC 0.08f
#define WP_GRID_MAJOR_FRAC 0.14f
#define WP_GRID_AXIS_FRAC 0.20f

static float grid_nice(float raw)
{
    float expn, f, n;

    if (!(raw > 0.0f) || !isfinite(raw))
        return 1.0f;
    expn = powf(10.0f, floorf(log10f(raw)));
    f = raw / expn;
    if (f <= 1.0f)
        n = 1.0f;
    else if (f <= 2.0f)
        n = 2.0f;
    else if (f <= 5.0f)
        n = 5.0f;
    else
        n = 10.0f;
    return n * expn;
}

static int axis_range(float a0, float a1, float step, int *lo, int *hi)
{
    int a, b;

    if (!(step > 1e-12f) || !isfinite(a0) || !isfinite(a1) || a1 < a0)
        return 0;
    a = (int)ceil((double)a0 / (double)step - 1e-6);
    b = (int)floor((double)a1 / (double)step + 1e-6);
    if (b < a)
        return 0;
    *lo = a;
    *hi = b;
    return b - a + 1;
}

static int line_kind(int n)
{
    if (n == 0)
        return 2;
    if (n % WP_GRID_MAJOR == 0)
        return 1;
    return 0;
}

static void kind_rgb(int kind, float rgb[3])
{
    if (kind == 2) {
        rgb[0] = 0.62f;
        rgb[1] = 0.64f;
        rgb[2] = 0.72f;
    } else if (kind == 1) {
        rgb[0] = 0.50f;
        rgb[1] = 0.52f;
        rgb[2] = 0.56f;
    } else {
        rgb[0] = 0.36f;
        rgb[1] = 0.38f;
        rgb[2] = 0.42f;
    }
}

static float kind_width(int kind, float step)
{
    float f, w;

    if (kind == 2)
        f = WP_GRID_AXIS_FRAC;
    else if (kind == 1)
        f = WP_GRID_MAJOR_FRAC;
    else
        f = WP_GRID_MINOR_FRAC;
    w = f * step;
    if (w > 0.35f * step)
        w = 0.35f * step;
    if (w < 1e-5f)
        w = 1e-5f;
    return w;
}

/* DEM cell winding: v00, v01, v11 and v00, v11, v10. CCW from +Y. */
static void emit_xz_quad(struct wp_vn_vertex *v, uint16_t *idx, uint32_t *nv, uint32_t *ni,
                         float x0, float x1, float z0, float z1, float y, const float rgb[3])
{
    uint16_t b = (uint16_t)*nv;
    float xs[4] = { x0, x1, x0, x1 };
    float zs[4] = { z0, z0, z1, z1 };
    uint32_t k;

    for (k = 0; k < 4; k++) {
        struct wp_vn_vertex *p = &v[*nv];
        p->px = xs[k];
        p->py = y;
        p->pz = zs[k];
        p->nx = 0.0f;
        p->ny = 1.0f;
        p->nz = 0.0f;
        p->r = rgb[0];
        p->g = rgb[1];
        p->b = rgb[2];
        p->u = (k == 1 || k == 3) ? 1.0f : 0.0f;
        p->v = (k >= 2) ? 1.0f : 0.0f;
        (*nv)++;
    }
    idx[(*ni)++] = b;
    idx[(*ni)++] = (uint16_t)(b + 2);
    idx[(*ni)++] = (uint16_t)(b + 3);
    idx[(*ni)++] = b;
    idx[(*ni)++] = (uint16_t)(b + 3);
    idx[(*ni)++] = (uint16_t)(b + 1);
}

int wp_grid_from_aabb(const struct wp_aabb *box, struct wp_grid *g)
{
    float spanx, spanz, span, raw, step, pad, r;
    int nx, nz, ix0, ix1, iz0, iz1, guard;

    if (!box || !g || !wp_aabb_ok(box))
        return -EINVAL;
    spanx = box->max[0] - box->min[0];
    spanz = box->max[2] - box->min[2];
    if (spanx < 1e-3f)
        spanx = 1.0f;
    if (spanz < 1e-3f)
        spanz = 1.0f;
    span = spanx > spanz ? spanx : spanz;
    raw = span / (float)WP_GRID_TARGET;
    step = grid_nice(raw);
    pad = step;
    r = wp_aabb_radius(box);
    if (r < step)
        r = step;
    g->y = box->min[1] - 0.02f * r;
    g->x0 = box->min[0] - pad;
    g->x1 = box->max[0] + pad;
    g->z0 = box->min[2] - pad;
    g->z1 = box->max[2] + pad;
    for (guard = 0; guard < 16; guard++) {
        nx = axis_range(g->x0, g->x1, step, &ix0, &ix1);
        nz = axis_range(g->z0, g->z1, step, &iz0, &iz1);
        if (nx >= 2 && nz >= 2 && nx + nz <= WP_GRID_MAX_LINES)
            break;
        step *= 2.0f;
        if (step > span * 4.0f)
            break;
    }
    nx = axis_range(g->x0, g->x1, step, &ix0, &ix1);
    nz = axis_range(g->z0, g->z1, step, &iz0, &iz1);
    if (nx < 2 || nz < 2)
        return -EINVAL;
    g->step = step;
    g->ix0 = ix0;
    g->ix1 = ix1;
    g->iz0 = iz0;
    g->iz1 = iz1;
    return 0;
}

int wp_grid_tessellate(const struct wp_grid *g, struct wp_mesh_cpu *out)
{
    uint32_t nlines, nv, ni, nvi, nii;
    int n, kind;
    float rgb[3], w, x, z;

    if (!g || !out || !(g->step > 0.0f) || g->ix1 < g->ix0 || g->iz1 < g->iz0)
        return -EINVAL;
    nlines = (uint32_t)(g->ix1 - g->ix0 + 1 + g->iz1 - g->iz0 + 1);
    if (nlines < 4 || nlines > WP_GRID_MAX_LINES)
        return -EINVAL;
    nv = nlines * 4u;
    ni = nlines * 6u;
    if (nv > WP_MESH_MAX_V || ni > WP_MESH_MAX_I)
        return -E2BIG;
    memset(out, 0, sizeof(*out));
    out->v = calloc(nv, sizeof(*out->v));
    out->idx = calloc(ni, sizeof(*out->idx));
    if (!out->v || !out->idx) {
        wp_mesh_cpu_free(out);
        return -ENOMEM;
    }
    nvi = 0;
    nii = 0;
    for (n = g->ix0; n <= g->ix1; n++) {
        kind = line_kind(n);
        kind_rgb(kind, rgb);
        w = kind_width(kind, g->step) * 0.5f;
        x = (float)n * g->step;
        emit_xz_quad(out->v, out->idx, &nvi, &nii, x - w, x + w, g->z0, g->z1, g->y, rgb);
    }
    for (n = g->iz0; n <= g->iz1; n++) {
        kind = line_kind(n);
        kind_rgb(kind, rgb);
        w = kind_width(kind, g->step) * 0.5f;
        z = (float)n * g->step;
        emit_xz_quad(out->v, out->idx, &nvi, &nii, g->x0, g->x1, z - w, z + w, g->y, rgb);
    }
    out->nv = nvi;
    out->ni = nii;
    return 0;
}

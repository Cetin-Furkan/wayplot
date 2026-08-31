#include "helper/math3d.h"
#include "renderer/camera.h"
#include "renderer/mesh.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_fail;

static void expect(int cond, const char *what)
{
    if (cond)
        printf("PASS  %s\n", what);
    else {
        printf("FAIL  %s\n", what);
        g_fail++;
    }
}

static const char *face_name[6] = { "+Z", "-Z", "+X", "-X", "+Y", "-Y" };

/* Geometric "camera sees the outward side" vs raster front-facing. These must
 * match. Disagreement is the dart: the GPU draws the inside of the cube. */
static void check_camera(const char *label, const struct wp_camera *cam, float aspect,
                         const struct wp_vn_vertex *vtx, const uint16_t *idx)
{
    float vp[16];
    int f, nfront = 0, nback = 0, nedge = 0, mismatch = 0;
    char what[160];

    wp_camera_vp(cam, aspect, vp);
    for (f = 0; f < 6; f++) {
        const struct wp_vn_vertex *a = &vtx[idx[f * 6]];
        const struct wp_vn_vertex *b = &vtx[idx[f * 6 + 1]];
        const struct wp_vn_vertex *c = &vtx[idx[f * 6 + 2]];
        const struct wp_vn_vertex *d = &vtx[idx[f * 6 + 5]];
        float pa[3] = { a->px, a->py, a->pz };
        float pb[3] = { b->px, b->py, b->pz };
        float pc[3] = { c->px, c->py, c->pz };
        float center[3] = {
            0.25f * (a->px + b->px + c->px + d->px),
            0.25f * (a->py + b->py + c->py + d->py),
            0.25f * (a->pz + b->pz + c->pz + d->pz),
        };
        float to_eye[3] = {
            cam->eye[0] - center[0],
            cam->eye[1] - center[1],
            cam->eye[2] - center[2],
        };
        float n[3] = { a->nx, a->ny, a->nz };
        float toward = wp_vec3_dot(n, to_eye);
        float area = wp_triangle_ndc_area(vp, pa, pb, pc);
        int geom_front = toward > 1e-4f;
        int rast_front = wp_triangle_front_facing(vp, pa, pb, pc);
        int solid = fabsf(area) > 1e-4f;

        printf("      %s face %s  toward %+0.3f  ndc_area %+0.5f  %s\n",
               label, face_name[f], toward, area,
               rast_front && solid ? "FRONT (drawn)" : (solid ? "BACK (culled)" : "edge"));

        if (!solid) {
            nedge++;
            continue;
        }
        if (geom_front != rast_front)
            mismatch++;
        if (rast_front)
            nfront++;
        else
            nback++;
    }

    snprintf(what, sizeof(what),
             "%s: raster front matches outward-normal vs eye (no inside-out dart)", label);
    expect(mismatch == 0, what);
    snprintf(what, sizeof(what), "%s: outside a cube the camera sees 1-3 faces, not 0 or 6", label);
    expect(nfront >= 1 && nfront <= 3, what);
    snprintf(what, sizeof(what), "%s: at least one face is back-face culled (not two-sided)", label);
    expect(nback >= 1, what);
    (void)nedge;
}

int main(void)
{
    struct wp_vn_vertex vtx[24];
    uint16_t idx[36];
    uint32_t nv = 0, ni = 0, f;
    struct wp_camera cam, headon, behind;
    float aspect = 16.0f / 9.0f;

    wp_cube_cpu(vtx, idx, &nv, &ni);
    expect(nv == 24 && ni == 36, "unit cube is 24 verts / 36 indices");
    {
        struct wp_aabb box;
        float c[3];

        expect(wp_aabb_from_vn(&box, vtx, 0) == -EINVAL, "empty verts have no AABB");
        expect(wp_aabb_from_vn(&box, vtx, nv) == 0, "cube AABB");
        expect(fabsf(box.min[0] + 0.5f) < 1e-5f && fabsf(box.max[0] - 0.5f) < 1e-5f &&
                   fabsf(box.min[1] + 0.5f) < 1e-5f && fabsf(box.max[1] - 0.5f) < 1e-5f &&
                   fabsf(box.min[2] + 0.5f) < 1e-5f && fabsf(box.max[2] - 0.5f) < 1e-5f,
               "unit cube AABB is [-0.5,0.5]^3");
        wp_aabb_center(&box, c);
        expect(fabsf(c[0]) + fabsf(c[1]) + fabsf(c[2]) < 1e-5f, "unit cube center is origin");
        expect(fabsf(wp_aabb_radius(&box) - 0.5f * sqrtf(3.0f)) < 1e-4f, "unit cube radius is half-diagonal");
    }
    {
        struct wp_vn_vertex *pts;
        struct wp_aabb box;
        struct timespec ts;
        uint32_t i, n = 16384;
        uint64_t t0, t1;

        pts = calloc(n, sizeof(*pts));
        expect(pts != NULL, "AABB bench verts");
        if (pts) {
            for (i = 0; i < n; i++) {
                pts[i].px = (float)(i % 128);
                pts[i].py = (float)(i / 128);
                pts[i].pz = 0.25f;
            }
            clock_gettime(CLOCK_MONOTONIC, &ts);
            t0 = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
            expect(wp_aabb_from_vn(&box, pts, n) == 0, "AABB 128^2");
            clock_gettime(CLOCK_MONOTONIC, &ts);
            t1 = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
            printf("      aabb 16384 verts  %.2f ns/vert\n", (double)(t1 - t0) / (double)n);
            expect(fabsf(box.max[0] - 127.0f) < 1e-4f && fabsf(box.max[1] - 127.0f) < 1e-4f,
                   "AABB tracked the far corner");
            free(pts);
        }
    }

    for (f = 0; f < 6; f++) {
        const struct wp_vn_vertex *a = &vtx[idx[f * 6]];
        const struct wp_vn_vertex *b = &vtx[idx[f * 6 + 1]];
        const struct wp_vn_vertex *c = &vtx[idx[f * 6 + 2]];
        float e1[3] = { b->px - a->px, b->py - a->py, b->pz - a->pz };
        float e2[3] = { c->px - a->px, c->py - a->py, c->pz - a->pz };
        float cr[3], n[3] = { a->nx, a->ny, a->nz };
        wp_vec3_cross(cr, e1, e2);
        expect(wp_vec3_dot(cr, n) > 0.0f, "each face is CCW from the outside");
        expect(fabsf(n[0]) + fabsf(n[1]) + fabsf(n[2]) > 0.5f, "face has an outward normal");
    }

    wp_camera_default(&cam);
    check_camera("default 3/4", &cam, aspect, vtx, idx);

    /* Head-on +Z: only the red face is drawn. */
    wp_camera_default(&headon);
    headon.eye[0] = 0.0f;
    headon.eye[1] = 0.0f;
    headon.eye[2] = 2.5f;
    check_camera("head-on +Z", &headon, aspect, vtx, idx);
    {
        float vp[16], a[3], b[3], c[3];
        wp_camera_vp(&headon, aspect, vp);
        a[0] = vtx[0].px;
        a[1] = vtx[0].py;
        a[2] = vtx[0].pz;
        b[0] = vtx[1].px;
        b[1] = vtx[1].py;
        b[2] = vtx[1].pz;
        c[0] = vtx[2].px;
        c[1] = vtx[2].py;
        c[2] = vtx[2].pz;
        expect(wp_triangle_front_facing(vp, a, b, c), "head-on: +Z is front");
        /* -Z is verts 4..7 */
        a[0] = vtx[4].px;
        a[1] = vtx[4].py;
        a[2] = vtx[4].pz;
        b[0] = vtx[5].px;
        b[1] = vtx[5].py;
        b[2] = vtx[5].pz;
        c[0] = vtx[6].px;
        c[1] = vtx[6].py;
        c[2] = vtx[6].pz;
        expect(!wp_triangle_front_facing(vp, a, b, c), "head-on: -Z is culled (camera cannot see it)");
    }

    wp_camera_default(&behind);
    behind.eye[0] = 0.0f;
    behind.eye[1] = 0.0f;
    behind.eye[2] = -2.5f;
    check_camera("head-on -Z", &behind, aspect, vtx, idx);

    /* Walk around the cube: from outside, always 1-3 front faces. */
    {
        int step, bad = 0;
        for (step = 0; step < 12; step++) {
            float ang = (float)step * 0.523598775f; /* 30 deg */
            struct wp_camera orbit;
            float vp[16];
            int nfront = 0;

            wp_camera_default(&orbit);
            orbit.eye[0] = cosf(ang) * 2.4f;
            orbit.eye[1] = 1.1f;
            orbit.eye[2] = sinf(ang) * 2.4f;
            wp_camera_vp(&orbit, aspect, vp);
            for (f = 0; f < 6; f++) {
                float pa[3] = { vtx[idx[f * 6]].px, vtx[idx[f * 6]].py, vtx[idx[f * 6]].pz };
                float pb[3] = { vtx[idx[f * 6 + 1]].px, vtx[idx[f * 6 + 1]].py, vtx[idx[f * 6 + 1]].pz };
                float pc[3] = { vtx[idx[f * 6 + 2]].px, vtx[idx[f * 6 + 2]].py, vtx[idx[f * 6 + 2]].pz };
                if (wp_triangle_front_facing(vp, pa, pb, pc))
                    nfront++;
            }
            if (nfront < 1 || nfront > 3)
                bad++;
        }
        expect(bad == 0, "orbit: always 1-3 front faces (never none, never two-sided)");
    }

    printf("%s  %d failure(s)\n", g_fail ? "RESULT FAIL" : "RESULT PASS", g_fail);
    return g_fail ? 1 : 0;
}

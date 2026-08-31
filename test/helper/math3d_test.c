#include "helper/math3d.h"
#include "renderer/camera.h"
#include "renderer/mesh.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

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

static int nearly(float a, float b, float eps)
{
    return fabsf(a - b) < eps;
}

int main(void)
{
    float id[16], r[16], v[4], o[4], proj[16];
    struct wp_camera cam;
    struct wp_vn_vertex vtx[24];
    uint16_t idx[36];
    uint32_t nv = 0, ni = 0, f;
    float origin[4] = { 0, 0, 0, 1 };
    float clip[4], ndc[3];
    float vp[16];
    float xmin = 1e9f, xmax = -1e9f, ymin = 1e9f, ymax = -1e9f;

    wp_mat4_identity(id);
    v[0] = 2;
    v[1] = 3;
    v[2] = 4;
    v[3] = 1;
    wp_mat4_mul_vec4(o, id, v);
    expect(nearly(o[0], 2, 1e-5f) && nearly(o[1], 3, 1e-5f) && nearly(o[2], 4, 1e-5f),
           "identity * vec");

    /* Column-major mul is c0*x + c1*y + c2*z + c3*w — same formula as the shader. */
    {
        float m[16];
        float cols[4];
        int i;
        wp_mat4_identity(m);
        m[0] = 2;
        m[5] = 3;
        m[10] = 4;
        m[12] = 5;
        m[13] = 6;
        m[14] = 7;
        v[0] = 1;
        v[1] = 1;
        v[2] = 1;
        v[3] = 1;
        wp_mat4_mul_vec4(o, m, v);
        for (i = 0; i < 4; i++)
            cols[i] = m[0 * 4 + i] * v[0] + m[1 * 4 + i] * v[1] +
                      m[2 * 4 + i] * v[2] + m[3 * 4 + i] * v[3];
        expect(nearly(o[0], cols[0], 1e-6f) && nearly(o[1], cols[1], 1e-6f) &&
                   nearly(o[2], cols[2], 1e-6f) && nearly(o[3], cols[3], 1e-6f),
               "mul_vec4 is column expansion (shader xform_cols)");
        expect(nearly(o[0], 7, 1e-5f) && nearly(o[1], 9, 1e-5f) && nearly(o[2], 11, 1e-5f),
               "affine columns: (2+5, 3+6, 4+7)");
    }

    wp_mat4_mul(r, id, id);
    expect(nearly(r[0], 1, 1e-5f) && nearly(r[15], 1, 1e-5f), "identity * identity");

    {
        float om[16];
        float p0[4] = { 0, 0, 0, 1 };
        float p1[4] = { 1280, 720, 0, 1 };
        wp_mat4_ortho_pixel(om, 1280, 720);
        wp_mat4_mul_vec4(clip, om, p0);
        wp_clip_to_ndc(ndc, clip);
        expect(nearly(ndc[0], -1, 1e-4f) && nearly(ndc[1], -1, 1e-4f),
               "pixel (0,0) top-left → NDC (-1,-1)");
        wp_mat4_mul_vec4(clip, om, p1);
        wp_clip_to_ndc(ndc, clip);
        expect(nearly(ndc[0], 1, 1e-4f) && nearly(ndc[1], 1, 1e-4f),
               "pixel (w,h) bottom-right → NDC (1,1)");
    }

    {
        float om[16];
        float p[4], clip_a[4], clip_b[4], na[3], nb[3];
        wp_mat4_ortho_vk(om, 2.0f, 1.0f, 0.1f, 20.0f);
        expect(om[5] < 0.0f, "world ortho Y-flips (m[5] < 0)");
        expect(om[11] == 0.0f && om[15] == 1.0f, "world ortho is parallel (w=1)");
        p[0] = 0.0f;
        p[1] = 2.0f;
        p[2] = -1.0f;
        p[3] = 1.0f;
        wp_mat4_mul_vec4(clip, om, p);
        wp_clip_to_ndc(ndc, clip);
        expect(nearly(ndc[1], -1.0f, 1e-4f), "view +Y at half_h → NDC y=-1 (top)");
        p[0] = 0.0f;
        p[1] = 0.0f;
        p[2] = -0.1f;
        p[3] = 1.0f;
        wp_mat4_mul_vec4(clip, om, p);
        wp_clip_to_ndc(ndc, clip);
        expect(nearly(ndc[2], 0.0f, 1e-4f), "ortho near plane ndc.z=0");
        p[2] = -20.0f;
        wp_mat4_mul_vec4(clip, om, p);
        wp_clip_to_ndc(ndc, clip);
        expect(nearly(ndc[2], 1.0f, 1e-4f), "ortho far plane ndc.z=1");
        p[0] = 1.0f;
        p[1] = 0.0f;
        p[2] = -1.0f;
        p[3] = 1.0f;
        wp_mat4_mul_vec4(clip_a, om, p);
        wp_clip_to_ndc(na, clip_a);
        p[2] = -10.0f;
        wp_mat4_mul_vec4(clip_b, om, p);
        wp_clip_to_ndc(nb, clip_b);
        expect(nearly(na[0], nb[0], 1e-5f) && nearly(na[1], nb[1], 1e-5f),
               "ortho: same view-xy at two depths share NDC xy");
        {
            float pm[16];
            wp_mat4_perspective_vk(pm, 1.04719755f, 1.0f, 0.1f, 20.0f);
            p[2] = -1.0f;
            wp_mat4_mul_vec4(clip_a, pm, p);
            wp_clip_to_ndc(na, clip_a);
            p[2] = -10.0f;
            wp_mat4_mul_vec4(clip_b, pm, p);
            wp_clip_to_ndc(nb, clip_b);
            expect(fabsf(na[0]) > fabsf(nb[0]) + 0.05f,
                   "perspective: far point is closer to NDC center than near");
        }
    }

    wp_camera_default(&cam);
    wp_camera_proj(&cam, 16.0f / 9.0f, proj);
    expect(proj[5] < 0.0f, "Vulkan projection Y-flips (m[5] < 0)");
    expect(wp_camera_front_clockwise() == (WP_FRONT_NDC_AREA_SIGN > 0),
           "CLOCKWISE only if raw NDC front sign is positive (Vulkan a = -shoelace)");
    expect(!wp_camera_front_clockwise(),
           "Y-flip + Vulkan a=-shoelace => FRONT_FACE_COUNTER_CLOCKWISE, not CLOCKWISE");

    wp_camera_vp(&cam, 16.0f / 9.0f, vp);
    wp_mat4_mul_vec4(clip, vp, origin);
    expect(clip[3] != 0.0f, "origin not at infinity");
    wp_clip_to_ndc(ndc, clip);
    expect(nearly(ndc[0], 0, 0.05f) && nearly(ndc[1], 0, 0.05f),
           "camera default: origin is center of NDC (not off to the side)");
    expect(ndc[2] > 0.0f && ndc[2] < 1.0f, "origin depth in (0,1)");
    printf("      origin ndc %.4f %.4f %.4f\n", ndc[0], ndc[1], ndc[2]);

    wp_cube_cpu(vtx, idx, &nv, &ni);
    expect(nv == 24 && ni == 36, "cpu cube 24 verts / 36 indices");

    for (f = 0; f < 6; f++) {
        const struct wp_vn_vertex *a = &vtx[idx[f * 6]];
        const struct wp_vn_vertex *b = &vtx[idx[f * 6 + 1]];
        const struct wp_vn_vertex *c = &vtx[idx[f * 6 + 2]];
        float e1[3] = { b->px - a->px, b->py - a->py, b->pz - a->pz };
        float e2[3] = { c->px - a->px, c->py - a->py, c->pz - a->pz };
        float cr[3];
        float n[3] = { a->nx, a->ny, a->nz };
        wp_vec3_cross(cr, e1, e2);
        expect(wp_vec3_dot(cr, n) > 0.0f, "face triangle is CCW with outward normal");
    }

    /* +Z face, first 4 verts. World CCW + Y-flip => clockwise NDC (negative area). */
    {
        float pndc[4][3];
        uint32_t i;
        float area, dx, dy;
        float a[3], b[3], c[3];
        for (i = 0; i < 4; i++) {
            float pv[4] = { vtx[i].px, vtx[i].py, vtx[i].pz, 1 };
            wp_mat4_mul_vec4(clip, vp, pv);
            expect(clip[3] > 0.0f, "+Z corner in front of near plane");
            wp_clip_to_ndc(pndc[i], clip);
        }
        area = wp_ndc_signed_area(pndc[0], pndc[1], pndc[2]);
        printf("      +Z face NDC area %.5f (want < 0 => Vulkan a > 0 => CCW front)\n", area);
        expect(area < 0.0f,
               "Y-flip: world-CCW +Z has negative raw NDC shoelace (Vulkan a > 0, CCW front)");
        a[0] = vtx[0].px;
        a[1] = vtx[0].py;
        a[2] = vtx[0].pz;
        b[0] = vtx[1].px;
        b[1] = vtx[1].py;
        b[2] = vtx[1].pz;
        c[0] = vtx[2].px;
        c[1] = vtx[2].py;
        c[2] = vtx[2].pz;
        expect(wp_triangle_front_facing(vp, a, b, c),
               "wp_triangle_front_facing agrees with clockwise NDC for visible +Z");
        dx = pndc[1][0] - pndc[0][0];
        dy = pndc[2][1] - pndc[1][1];
        expect(fabsf(dx) > 0.05f && fabsf(dy) > 0.05f,
               "+Z face is a rectangle in NDC, not a collapsed line");
    }

    for (f = 0; f < nv; f++) {
        float pv[4] = { vtx[f].px, vtx[f].py, vtx[f].pz, 1 };
        wp_mat4_mul_vec4(clip, vp, pv);
        wp_clip_to_ndc(ndc, clip);
        if (ndc[0] < xmin)
            xmin = ndc[0];
        if (ndc[0] > xmax)
            xmax = ndc[0];
        if (ndc[1] < ymin)
            ymin = ndc[1];
        if (ndc[1] > ymax)
            ymax = ndc[1];
    }
    printf("      cube NDC xy [%.3f,%.3f] x [%.3f,%.3f]\n", xmin, xmax, ymin, ymax);
    expect(xmax - xmin > 0.15f && ymax - ymin > 0.15f, "cube covers a real rectangle in NDC");
    expect((xmax - xmin) / (ymax - ymin) < 3.0f && (ymax - ymin) / (xmax - xmin) < 3.0f,
           "cube NDC aspect is not a needle/dart");
    expect(xmin > -1.2f && xmax < 1.2f && ymin > -1.2f && ymax < 1.2f,
           "cube sits in the middle of the view, not off in a corner");

    printf("%s  %d failure(s)\n", g_fail ? "RESULT FAIL" : "RESULT PASS", g_fail);
    return g_fail ? 1 : 0;
}

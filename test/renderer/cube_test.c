#define _GNU_SOURCE
#include "engine/present.h"
#include "helper/math3d.h"
#include "renderer/camera.h"
#include "renderer/lit.h"
#include "renderer/mesh.h"
#include "renderer/pass.h"

#include <errno.h>
#include <stdio.h>
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

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int main(void)
{
    struct wp_session s;
    struct wp_present p;
    struct wp_pass pass;
    struct wp_mesh mesh;
    struct wp_lit lit;
    struct wp_camera cam;
    struct wp_present_frame f;
    float model[16];
    uint64_t deadline, t0;
    unsigned presented = 0, polls = 0;
    int ret;

    memset(&pass, 0, sizeof(pass));
    memset(&mesh, 0, sizeof(mesh));
    memset(&lit, 0, sizeof(lit));
    wp_mat4_identity(model);
    wp_camera_default(&cam);

    ret = wp_session_open(&s);
    expect(ret == 0, "session_open");
    if (ret < 0)
        return 1;
    ret = wp_session_setup_surface(&s);
    expect(ret == 0, "surface + feedback");
    if (ret < 0) {
        wp_session_close(&s);
        return 1;
    }
    ret = wp_present_open(&p, &s);
    expect(ret == 0, "present_open");
    if (ret < 0) {
        wp_session_close(&s);
        return 1;
    }

    ret = wp_mesh_cube(&p.device, &mesh);
    expect(ret == 0 && mesh.index_count == 36, "upload cube as a mesh (not a spinning object)");
    {
        struct wp_vn_vertex vtx[24];
        uint16_t idx[36];
        uint32_t nv = 0, ni = 0, face, nfront = 0;
        float vp[16];
        wp_cube_cpu(vtx, idx, &nv, &ni);
        wp_camera_vp(&cam, (float)p.sc.width / (float)p.sc.height, vp);
        for (face = 0; face < 6; face++) {
            float a[3] = { vtx[idx[face * 6]].px, vtx[idx[face * 6]].py, vtx[idx[face * 6]].pz };
            float b[3] = { vtx[idx[face * 6 + 1]].px, vtx[idx[face * 6 + 1]].py, vtx[idx[face * 6 + 1]].pz };
            float c[3] = { vtx[idx[face * 6 + 2]].px, vtx[idx[face * 6 + 2]].py, vtx[idx[face * 6 + 2]].pz };
            if (wp_triangle_front_facing(vp, a, b, c))
                nfront++;
        }
        expect(nfront >= 1 && nfront <= 3,
               "CPU clip of the uploaded cube: camera sees 1-3 faces (not a dart, not two-sided)");
        expect(!wp_camera_front_clockwise(),
               "pipeline front face is COUNTER_CLOCKWISE (Vulkan a = -shoelace)");
    }
    ret = wp_pass_init(&pass, &p.device, p.negotiated.vk_format, p.sc.width, p.sc.height);
    expect(ret == 0, "pass (owns BeginRendering + depth)");
    if (ret < 0) {
        wp_mesh_destroy(&p.device, &mesh);
        wp_present_close(&p);
        wp_session_close(&s);
        return 1;
    }
    ret = wp_lit_init(&lit, &p.device, p.negotiated.vk_format);
    expect(ret == 0, "lit (front from camera, back cull, not two-sided)");
    if (ret < 0) {
        wp_pass_destroy(&pass);
        wp_mesh_destroy(&p.device, &mesh);
        wp_present_close(&p);
        wp_session_close(&s);
        return 1;
    }

    t0 = now_ns();
    deadline = t0 + 8000000000ull;
    while (presented < 8 && now_ns() < deadline && !s.closed) {
        float rot[16];
        polls++;
        ret = wp_present_poll(&p, 50ull * 1000ull * 1000ull);
        if (ret < 0) {
            g_fail++;
            break;
        }
        if (!wp_present_begin(&p, &f))
            continue;
        /* Motion lives in the caller, not in the mesh. */
        wp_mat4_rotate(rot, (float)(now_ns() - t0) / 1e9f * 0.6f, 0.2f, 1.0f, 0.15f);
        wp_pass_opaque_begin(&pass, f.cmd, f.view, f.extent.width, f.extent.height, f.slot);
        wp_lit_draw(&lit, f.cmd, f.extent.width, f.extent.height, f.slot, &mesh, &cam, rot);
        wp_pass_opaque_end(f.cmd);
        ret = wp_present_end(&p, &f);
        if (ret < 0) {
            g_fail++;
            break;
        }
        presented++;
    }
    printf("      lit-mesh frames %u  polls %u  %ux%u\n",
           presented, polls, p.sc.width, p.sc.height);
    expect(presented >= 3, "drew the cube mesh on at least three swapchain images");

    wp_lit_destroy(&lit);
    wp_pass_destroy(&pass);
    wp_mesh_destroy(&p.device, &mesh);
    wp_present_close(&p);
    wp_session_close(&s);
    printf("%s  %d failure(s)\n", g_fail ? "RESULT FAIL" : "RESULT PASS", g_fail);
    return g_fail ? 1 : 0;
}

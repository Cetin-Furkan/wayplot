#include "test_framework.h"
#include "khoros/gfx/blob.h"
#include "khoros/core/topology.h"

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

[[nodiscard]]
bool test_blob_self_relative_box(void) {
    uint8_t buf[512];
    memset(buf, 0xFF, sizeof(buf));
    size_t n = khr_blob_write_box(buf, sizeof(buf));
    TEST_ASSERT(n == khr_blob_bytes(8, 36), "box blob size");
    khr_blob_view_t v = {};
    TEST_ASSERT(khr_blob_parse(buf, n, &v), "box parses");
    TEST_ASSERT_EQ(v.vert_count, 8U, "8 verts");
    TEST_ASSERT_EQ(v.index_count, 36U, "36 indices");
    TEST_ASSERT(v.verts != nullptr && v.indices != nullptr, "relptrs resolved");
    TEST_ASSERT(v.verts_byte_off >= sizeof(khr_blob_t), "verts after header");
    TEST_ASSERT(v.indices[0] < 8U && v.indices[35] < 8U, "indices in range");
    TEST_ASSERT(khr_blob_write_box(buf, 16) == 0, "undersize rejected");
    buf[0] ^= 0xFF;
    TEST_ASSERT(!khr_blob_parse(buf, n, &v), "bad magic rejected");
    return true;
}

[[nodiscard]]
bool test_blob_ingest_leaves_ui_reserve(void) {
    khr_topology_t topo = {};
    TEST_ASSERT(khr_topology_init(&topo), "topology init");
    TEST_ASSERT(topo.hugepage != nullptr, "hugepage");
    size_t pay = khr_hp_payload_cap(topo.hugepage_sz);
    size_t ui = khr_hp_ui_off(topo.hugepage_sz);
    TEST_ASSERT(pay > 0 && ui == pay, "split");
    memset((uint8_t*)topo.hugepage + ui, 0xA5, KHR_HP_UI_RESERVE);

    uint8_t blob[512];
    size_t bn = khr_blob_write_box(blob, sizeof(blob));
    TEST_ASSERT(bn > 0, "write box");

    char path[] = "/tmp/khoros_blob_XXXXXX";
    int fd = mkstemp(path);
    TEST_ASSERT(fd >= 0, "mkstemp");
    TEST_ASSERT(write(fd, blob, bn) == (ssize_t)bn, "write blob file");
    TEST_ASSERT(fsync(fd) == 0, "fsync");
    close(fd);

    size_t n = 0;
    bool ok = khr_topology_ingest(&topo, path, &n);
    bool ui_ok = true;
    uint8_t* u = (uint8_t*)topo.hugepage + ui;
    for (size_t i = 0; i < KHR_HP_UI_RESERVE; i++) {
        if (u[i] != 0xA5) {
            ui_ok = false;
            break;
        }
    }
    khr_blob_view_t v = {};
    bool parsed = khr_blob_parse(topo.hugepage, pay, &v);
    khr_topology_destroy(&topo);
    unlink(path);
    TEST_ASSERT(ok && n == bn, "ingest blob");
    TEST_ASSERT(parsed && v.vert_count == 8U, "payload is the box");
    TEST_ASSERT(ui_ok, "UI reserve untouched");
    return true;
}

[[nodiscard]]
bool test_blob_fit_centers_offset_mesh(void) {
    const float xyz[9] = { 10.0f, 0.0f, 0.0f, 11.0f, 0.0f, 0.0f, 10.0f, 1.0f, 0.0f };
    const uint32_t idx[3] = { 0, 1, 2 };
    uint8_t buf[128];
    size_t n = khr_blob_write(buf, sizeof(buf), xyz, 3, idx, 3);
    TEST_ASSERT(n == khr_blob_bytes(3, 3), "offset triangle blob size");
    khr_mesh_push_t push = {};
    TEST_ASSERT(khr_mesh_setup(buf, n, 0x1000, &push), "setup");
    TEST_ASSERT_EQ(push.vert_count, 3U, "3 verts");
    TEST_ASSERT_EQ(push.index_count, 3U, "3 idx");
    TEST_ASSERT(push.verts_addr == 0x1000 + sizeof(khr_blob_t), "verts BDA after header");
    float t2 = push.mvp_c3[0] * push.mvp_c3[0] +
               push.mvp_c3[1] * push.mvp_c3[1] +
               push.mvp_c3[2] * push.mvp_c3[2];
    TEST_ASSERT(t2 > 0.01f, "offset mesh is translated into view");
    uint8_t box[512];
    size_t bn = khr_blob_write_box(box, sizeof(box));
    khr_mesh_push_t box_p = {};
    TEST_ASSERT(khr_mesh_setup(box, bn, 0x2000, &box_p), "box setup");
    TEST_ASSERT(box_p.mvp_c3[0] * box_p.mvp_c3[0] +
                box_p.mvp_c3[1] * box_p.mvp_c3[1] < 0.01f,
                "centered box has near-zero xy translation");
    TEST_ASSERT(box_p.mvp_c3[2] > 0.4f && box_p.mvp_c3[2] < 0.6f,
                "Vulkan z maps to ~0.5 at the origin");
    return true;
}

[[nodiscard]]
bool test_blob_box_outward_winding(void) {
    uint8_t buf[512];
    size_t n = khr_blob_write_box(buf, sizeof(buf));
    khr_blob_view_t v = {};
    TEST_ASSERT(khr_blob_parse(buf, n, &v), "parse");
    uint32_t outward = 0;
    for (uint32_t t = 0; t < v.index_count; t += 3) {
        const float* a = v.verts + v.indices[t] * 3U;
        const float* b = v.verts + v.indices[t + 1U] * 3U;
        const float* c = v.verts + v.indices[t + 2U] * 3U;
        float e0[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
        float e1[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
        float nx = e0[1] * e1[2] - e0[2] * e1[1];
        float ny = e0[2] * e1[0] - e0[0] * e1[2];
        float nz = e0[0] * e1[1] - e0[1] * e1[0];
        float cx = (a[0] + b[0] + c[0]) / 3.0f;
        float cy = (a[1] + b[1] + c[1]) / 3.0f;
        float cz = (a[2] + b[2] + c[2]) / 3.0f;
        if (nx * cx + ny * cy + nz * cz > 0.0f) {
            outward++;
        }
    }
    TEST_ASSERT_EQ(outward, 12U, "all 12 triangles face outward");
    return true;
}

[[nodiscard]]
bool test_cam_orbit_snap_and_pick(void) {
    khr_cam_t cam = {};
    khr_cam_frame(&cam, nullptr, 0, true);
    float yaw0 = cam.yaw;
    khr_cam_orbit(&cam, 0.2f, 10.0f);
    TEST_ASSERT(cam.yaw > yaw0, "orbit adds yaw");
    TEST_ASSERT(cam.pitch <= 1.52f + 1.0e-5f, "pitch clamps at +lim");
    khr_cam_orbit(&cam, 0.0f, -20.0f);
    TEST_ASSERT(cam.pitch >= -1.52f - 1.0e-5f, "pitch clamps at -lim");

    khr_cam_snap_axis(&cam, 3);
    TEST_ASSERT(cam.pitch > -0.01f && cam.pitch < 0.01f, "+Z snap is level");
    TEST_ASSERT(cam.yaw > 3.0f && cam.yaw < 3.3f, "+Z snap looks from +Z");
    TEST_ASSERT_EQ(khr_cam_pick_axis(&cam, -0.78f, 0.0f), 1, "+X arm");
    TEST_ASSERT_EQ(khr_cam_pick_axis(&cam, 0.78f, 0.0f), -1, "-X arm");
    TEST_ASSERT_EQ(khr_cam_pick_axis(&cam, 0.0f, -0.78f), 2, "+Y arm is up");
    TEST_ASSERT_EQ(khr_cam_pick_axis(&cam, 0.0f, 0.0f), 3, "hub is the view axis");

    khr_cam_snap_axis(&cam, 1);
    TEST_ASSERT(cam.yaw < -1.5f && cam.yaw > -1.6f, "+X snap looks from +X");
    float t0 = cam.target[0];
    khr_cam_pan(&cam, 40.0f, 0.0f);
    TEST_ASSERT(cam.target[0] != t0, "pan moves the target");
    float r0 = cam.radius;
    khr_cam_zoom(&cam, 1.0f);
    TEST_ASSERT(cam.radius < r0, "positive ticks zoom in");
    return true;
}

[[nodiscard]]
bool test_blob_gizmo_arm(void) {
    uint8_t buf[512];
    size_t n = khr_blob_write_gizmo_arm(buf, sizeof(buf));
    TEST_ASSERT(n == khr_blob_bytes(8, 36), "gizmo arm is a box blob");
    khr_blob_view_t v = {};
    TEST_ASSERT(khr_blob_parse(buf, n, &v), "gizmo arm parses");
    TEST_ASSERT_EQ(v.vert_count, 8U, "8 verts");
    TEST_ASSERT_EQ(v.index_count, 36U, "36 indices");
    khr_mesh_push_t push = {};
    khr_cam_t cam = {};
    khr_cam_frame(&cam, nullptr, 0, true);
    khr_cam_snap_axis(&cam, 3);
    push.verts_addr = 1;
    push.indices_addr = 2;
    push.index_count = 36;
    push.vert_count = 8;
    khr_gizmo_arm_apply(&push, cam.r0, cam.r1, cam.r2, 0, 0xFF);
    TEST_ASSERT_EQ(push.pad0, 0xFFU, "tint lands in pad0");
    TEST_ASSERT(push.mvp_c3[3] == 1.0f, "gizmo w is 1");
    return true;
}

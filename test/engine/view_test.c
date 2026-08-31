#define _GNU_SOURCE
#include "engine/hit.h"
#include "engine/input.h"
#include "engine/present.h"
#include "engine/view.h"
#include "helper/math3d.h"
#include "renderer/camera.h"
#include "renderer/lit.h"
#include "renderer/mesh.h"
#include "renderer/pass.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RW 128
#define RH 128

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

static uint8_t *px_at(uint8_t *p, int x, int y)
{
    return p + ((size_t)y * RW + (size_t)x) * 4;
}

static int is_clear(const uint8_t *p)
{
    return p[0] < 40 && p[1] > 20 && p[1] < 70 && p[2] > 30 && p[2] < 90 && p[0] + 8 < p[2];
}

static int is_plus_z(const uint8_t *p)
{
    return p[0] > 50 && p[0] > (unsigned)p[1] * 3 / 2 && p[0] > p[2];
}

/* Factory cube +Y face 0.28, 0.84, 0.84 after lighting. Not +Z red, not clear. */
static int is_plus_y(const uint8_t *p)
{
    return p[1] > 100 && p[2] > 100 && (int)p[1] > (int)p[0] + 40 &&
           (int)p[2] > (int)p[0] + 40;
}

static unsigned count_plus_y(const uint8_t *pix)
{
    unsigned n = 0;
    int i;

    for (i = 0; i < RW * RH; i++) {
        if (is_plus_y(pix + (size_t)i * 4))
            n++;
    }
    return n;
}

static void project_pt(const float vp[16], const float p[3], float ndc[3])
{
    float v[4] = { p[0], p[1], p[2], 1.0f };
    float clip[4];

    wp_mat4_mul_vec4(clip, vp, v);
    wp_clip_to_ndc(ndc, clip);
}

static int render_view(struct wp_device *d, struct wp_pass *pass, struct wp_lit *lit,
                       struct wp_mesh *mesh, struct wp_view *views, uint32_t nviews,
                       const float model[16], uint8_t *pixels)
{
    VkImage img = VK_NULL_HANDLE;
    VkImageView iv = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { RW, RH, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_HOST_TRANSFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkDeviceImageMemoryRequirements q = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS,
        .pCreateInfo = &ici,
    };
    VkMemoryRequirements2 memreq = { .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
    VkPhysicalDeviceMemoryProperties mprops;
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    VkCommandBufferAllocateInfo cai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
        .commandPool = d->pool,
    };
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VkImageMemoryBarrier2 b1 = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    VkImageMemoryBarrier2 b2 = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
        .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    VkDependencyInfo dep = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
    };
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkImageToMemoryCopy region = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_TO_MEMORY_COPY,
        .pHostPointer = pixels,
        .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .imageExtent = { RW, RH, 1 },
    };
    VkCopyImageToMemoryInfo cpy = {
        .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_MEMORY_INFO,
        .srcImageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .regionCount = 1,
        .pRegions = &region,
    };
    int32_t vx, vy;
    uint32_t vw, vh, idx;
    int ret = -EIO;

    if (!d->host_image_copy)
        return -ENOTSUP;
    vkGetDeviceImageMemoryRequirements(d->device, &q, &memreq);
    if (vkCreateImage(d->device, &ici, NULL, &img) != VK_SUCCESS)
        return -EIO;
    vkGetPhysicalDeviceMemoryProperties(d->phy, &mprops);
    idx = wp_find_memory_type(&mprops, memreq.memoryRequirements.memoryTypeBits,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (idx == UINT32_MAX)
        idx = wp_find_memory_type(&mprops, memreq.memoryRequirements.memoryTypeBits, 0);
    if (idx == UINT32_MAX)
        goto out;
    ai.allocationSize = memreq.memoryRequirements.size;
    ai.memoryTypeIndex = idx;
    if (vkAllocateMemory(d->device, &ai, NULL, &mem) != VK_SUCCESS)
        goto out;
    if (vkBindImageMemory(d->device, img, mem, 0) != VK_SUCCESS)
        goto out;
    vci.image = img;
    if (vkCreateImageView(d->device, &vci, NULL, &iv) != VK_SUCCESS)
        goto out;
    if (vkAllocateCommandBuffers(d->device, &cai, &cmd) != VK_SUCCESS)
        goto out;

    vkBeginCommandBuffer(cmd, &bi);
    b1.image = img;
    dep.pImageMemoryBarriers = &b1;
    vkCmdPipelineBarrier2(cmd, &dep);
    wp_pass_opaque_begin(pass, cmd, iv, RW, RH, 0);
    wp_lit_reset(lit, 0);
    {
        uint32_t i;
        for (i = 0; i < nviews; i++) {
            if (wp_view_bind(&views[i], cmd, 1, RW, RH) < 0)
                goto out;
            if (wp_view_pixels(&views[i], 1, RW, RH, &vx, &vy, &vw, &vh) < 0)
                goto out;
            if (!wp_view_mesh_on(&views[i], 0))
                continue;
            wp_lit_draw(lit, cmd, vw, vh, 0, mesh, &views[i].cam, model);
        }
    }
    wp_pass_opaque_end(cmd);
    b2.image = img;
    dep.pImageMemoryBarriers = &b2;
    vkCmdPipelineBarrier2(cmd, &dep);
    vkEndCommandBuffer(cmd);
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (vkQueueSubmit(d->gfx, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS)
        goto out;
    if (vkQueueWaitIdle(d->gfx) != VK_SUCCESS)
        goto out;
    cpy.srcImage = img;
    if (vkCopyImageToMemory(d->device, &cpy) != VK_SUCCESS)
        goto out;
    ret = 0;
out:
    if (cmd)
        vkFreeCommandBuffers(d->device, d->pool, 1, &cmd);
    if (iv)
        vkDestroyImageView(d->device, iv, NULL);
    if (img)
        vkDestroyImage(d->device, img, NULL);
    if (mem)
        vkFreeMemory(d->device, mem, NULL);
    return ret;
}

static void cpu_tests(void)
{
    struct wp_view v;
    struct wp_camera def;
    struct wp_input in;
    struct wp_pointer p;
    struct wp_hit_stack hits;
    struct wp_rect pane = { 0, 152, 1280, 568 };
    struct wp_rect card = { 24, 56, 200, 88 };
    float c0[3], d0, yaw0;
    int32_t x, y;
    uint32_t w, h;
    uint32_t k;
    uint64_t t0, t1;

    wp_view_init(&v);
    wp_camera_default(&def);
    expect(fabsf(v.cam.eye[0] - def.eye[0]) < 1e-4f && fabsf(v.cam.eye[1] - def.eye[1]) < 1e-4f &&
               fabsf(v.cam.eye[2] - def.eye[2]) < 1e-4f,
           "init eye matches wp_camera_default");
    expect(v.dist > 2.0f && v.dist < 4.0f, "default distance is the corner view");
    expect(wp_view_mesh_on(&v, 0) && wp_view_mesh_on(&v, 31), "init layers are all on");
    v.layers = 0;
    expect(!wp_view_mesh_on(&v, 0), "layers 0 draws no mesh");
    v.layers = 1u;
    expect(wp_view_mesh_on(&v, 0) && !wp_view_mesh_on(&v, 1), "bit 0 only");
    expect(!wp_view_mesh_on(&v, 32), "mesh index 32 is off");
    wp_view_init(&v);
    c0[0] = v.center[0];
    c0[1] = v.center[1];
    c0[2] = v.center[2];
    d0 = v.dist;
    yaw0 = v.yaw;
    wp_view_orbit(&v, 20.0f, 0.0f);
    expect(v.yaw > yaw0, "drag right increases yaw");
    expect(fabsf(v.dist - d0) < 1e-6f, "orbit does not change distance");
    expect(fabsf(v.center[0] - c0[0]) + fabsf(v.center[1] - c0[1]) + fabsf(v.center[2] - c0[2]) < 1e-6f,
           "orbit does not move the look-at");
    expect(fabsf(v.cam.center[0] - v.center[0]) < 1e-6f, "camera.center follows the view");

    wp_view_init(&v);
    wp_view_orbit(&v, 0.0f, -10000.0f);
    expect(v.pitch <= WP_VIEW_PITCH_LIM && v.pitch >= -WP_VIEW_PITCH_LIM, "pitch clamped (look up)");
    wp_view_orbit(&v, 0.0f, 20000.0f);
    expect(v.pitch >= -WP_VIEW_PITCH_LIM, "pitch clamped (look down)");

    wp_view_init(&v);
    v.rect = (struct wp_rect){ 10, 20, 30, 40 };
    expect(wp_view_pixels(&v, 2, 200, 200, &x, &y, &w, &h) == 0 && x == 20 && y == 40 && w == 60 &&
               h == 80,
           "buffer pixels = logical × scale");
    v.rect = (struct wp_rect){ 0, 0, 0, 10 };
    expect(wp_view_pixels(&v, 1, 100, 100, &x, &y, &w, &h) == -EINVAL, "empty view rejected");

    wp_hit_clear(&hits);
    expect(wp_hit_push(&hits, 1, card) == 0, "card on the stack");
    wp_input_init(&in);
    memset(&p, 0, sizeof(p));
    p.inside = 1;
    p.pressed = WP_INPUT_LEFT;
    p.x = 640;
    p.y = 400;
    wp_input_feed(&in, &p, &hits, &pane, 1, 1280, 720, 0);
    expect(in.orbiting && in.chrome == WP_CHROME_NONE && in.drag_id == 0, "press in the pane orbits");
    p.pressed = 0;
    p.x = 660;
    wp_input_feed(&in, &p, &hits, &pane, 1, 1280, 720, 0);
    expect(in.orbiting && in.orbit_dx == 20, "orbit is sticky and reports dx");
    p.x = 50;
    p.y = 80; /* over the card, still orbiting */
    wp_input_feed(&in, &p, &hits, &pane, 1, 1280, 720, 0);
    expect(in.orbiting && in.drag_id == 0, "orbit does not re-pick a card");
    p.released = WP_INPUT_LEFT;
    wp_input_feed(&in, &p, &hits, &pane, 1, 1280, 720, 0);
    expect(!in.orbiting, "release ends orbit");

    wp_input_init(&in);
    memset(&p, 0, sizeof(p));
    p.inside = 1;
    p.pressed = WP_INPUT_LEFT;
    p.x = 640;
    p.y = 20;
    wp_input_feed(&in, &p, &hits, &pane, 1, 1280, 720, 0);
    expect(in.chrome == WP_CHROME_MOVE && !in.orbiting, "move band still moves the window");

    wp_input_init(&in);
    p.x = 40;
    p.y = 80;
    p.pressed = WP_INPUT_LEFT;
    wp_input_feed(&in, &p, &hits, &pane, 1, 1280, 720, 0);
    expect(in.drag_id == 1 && !in.orbiting, "card still drags over the view");

    wp_view_init(&v);
    d0 = v.dist;
    c0[0] = v.center[0];
    c0[1] = v.center[1];
    c0[2] = v.center[2];
    wp_view_pan(&v, 40.0f, 0.0f);
    expect(fabsf(v.dist - d0) < 1e-6f, "pan does not change distance");
    expect(fabsf(v.center[0] - c0[0]) + fabsf(v.center[1] - c0[1]) + fabsf(v.center[2] - c0[2]) > 1e-4f,
           "pan moves the look-at");
    wp_view_init(&v);
    d0 = v.dist;
    c0[0] = v.center[0];
    wp_view_dolly(&v, 256 * 10);
    expect(v.dist > d0, "positive axis zooms out");
    expect(fabsf(v.center[0] - c0[0]) < 1e-5f, "dolly does not move the look-at");
    wp_view_dolly(&v, -256 * 10000);
    expect(v.dist >= WP_VIEW_DIST_MIN, "dolly clamp near");
    wp_view_dolly(&v, 256 * 10000);
    expect(v.dist <= WP_VIEW_DIST_MAX, "dolly clamp far");

    wp_input_init(&in);
    memset(&p, 0, sizeof(p));
    p.inside = 1;
    p.x = 640;
    p.y = 400;
    p.pressed = WP_INPUT_RIGHT;
    wp_input_feed(&in, &p, &hits, &pane, 1, 1280, 720, 0);
    expect(in.panning && !in.orbiting && in.drag_id == 0, "right press in the pane pans");
    p.pressed = 0;
    p.x = 650;
    wp_input_feed(&in, &p, &hits, &pane, 1, 1280, 720, 0);
    expect(in.panning && in.pan_dx == 10, "pan is sticky and reports dx");
    p.released = WP_INPUT_RIGHT;
    wp_input_feed(&in, &p, &hits, &pane, 1, 1280, 720, 0);
    expect(!in.panning, "right release ends pan");

    wp_input_init(&in);
    memset(&p, 0, sizeof(p));
    p.inside = 1;
    p.x = 640;
    p.y = 400;
    p.pressed = WP_INPUT_LEFT;
    wp_input_feed(&in, &p, &hits, &pane, 1, 1280, 720, 0);
    expect(in.orbiting && !in.panning, "left press still orbits (not pan)");

    wp_input_init(&in);
    memset(&p, 0, sizeof(p));
    p.inside = 1;
    p.x = 640;
    p.y = 400;
    p.axis_v = 512;
    wp_input_feed(&in, &p, &hits, &pane, 1, 1280, 720, 0);
    expect(in.axis_v == 512, "axis inside the view is forwarded");
    p.y = 20;
    p.axis_v = 512;
    wp_input_feed(&in, &p, &hits, &pane, 1, 1280, 720, 0);
    expect(in.axis_v == 0, "axis outside the view is ignored");

    {
        struct wp_rect split[2] = { { 0, 152, 640, 568 }, { 640, 152, 640, 568 } };
        wp_input_init(&in);
        memset(&p, 0, sizeof(p));
        p.inside = 1;
        p.pressed = WP_INPUT_LEFT;
        p.x = 800;
        p.y = 400;
        wp_input_feed(&in, &p, &hits, split, 2, 1280, 720, 0);
        expect(in.orbiting && in.view_i == 1, "press in the right pane selects view 1");
        p.x = 200;
        p.pressed = WP_INPUT_LEFT;
        wp_input_init(&in);
        wp_input_feed(&in, &p, &hits, split, 2, 1280, 720, 0);
        expect(in.orbiting && in.view_i == 0, "press in the left pane selects view 0");
    }

    wp_view_init(&v);
    wp_view_plan(&v);
    expect(v.plan, "plan flag");
    expect(fabsf(v.cam.eye[0] - v.center[0]) < 1e-5f && fabsf(v.cam.eye[2] - v.center[2]) < 1e-5f,
           "plan eye is above center");
    expect(v.cam.eye[1] > v.center[1] + 1.0f, "plan eye is on +Y");
    expect(fabsf(v.cam.up[1]) < 1e-5f, "plan up is not world +Y");
    {
        float f[3] = { v.cam.center[0] - v.cam.eye[0], v.cam.center[1] - v.cam.eye[1],
                       v.cam.center[2] - v.cam.eye[2] };
        float fn = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        float un = sqrtf(v.cam.up[0] * v.cam.up[0] + v.cam.up[1] * v.cam.up[1] +
                         v.cam.up[2] * v.cam.up[2]);
        float par = fabsf(f[0] * v.cam.up[0] + f[1] * v.cam.up[1] + f[2] * v.cam.up[2]);
        expect(fn > 0.5f && un > 0.5f && par / (fn * un) < 0.1f, "plan up is not parallel to the view");
    }
    yaw0 = v.yaw;
    wp_view_orbit(&v, 40.0f, 0.0f);
    expect(v.plan && v.yaw > yaw0, "yaw stays in plan");
    expect(fabsf(v.cam.eye[0] - v.center[0]) < 1e-5f && fabsf(v.cam.eye[2] - v.center[2]) < 1e-5f,
           "yaw in plan keeps the eye above");
    {
        float px = v.center[0], py = v.center[1], pz = v.center[2];
        wp_view_pan(&v, 40.0f, 0.0f);
        expect(fabsf(v.center[1] - py) < 1e-4f, "plan pan keeps height");
        expect(fabsf(v.center[0] - px) + fabsf(v.center[2] - pz) > 1e-4f, "plan pan moves XZ");
    }
    wp_view_init(&v);
    wp_view_plan(&v);
    expect(v.cam.ortho, "plan is orthographic");
    {
        float vp[16], n_hi[3], n_lo[3];
        float hi[3] = { 0.5f, 0.5f, 0.0f };
        float lo[3] = { 0.5f, -0.5f, 0.0f };
        uint64_t tproj0, tproj1;

        wp_camera_vp(&v.cam, 1.0f, vp);
        project_pt(vp, hi, n_hi);
        project_pt(vp, lo, n_lo);
        expect(fabsf(n_hi[0] - n_lo[0]) < 1e-4f && fabsf(n_hi[1] - n_lo[1]) < 1e-4f,
               "plan ortho: same XZ at two heights share NDC xy");
        expect(fabsf(n_hi[2] - n_lo[2]) > 1e-4f, "plan ortho still has depth");
        tproj0 = now_ns();
        for (k = 0; k < 100000; k++)
            wp_camera_proj(&v.cam, 1.0f, vp);
        tproj1 = now_ns();
        printf("      ortho proj × 100000  %.2f ns/call\n", (double)(tproj1 - tproj0) / 100000.0);
        v.cam.ortho = 0;
        wp_camera_vp(&v.cam, 1.0f, vp);
        project_pt(vp, hi, n_hi);
        project_pt(vp, lo, n_lo);
        expect(fabsf(n_hi[0]) > fabsf(n_lo[0]) + 0.02f,
               "same eye with perspective: near XZ is magnified");
        v.cam.ortho = 1;
    }
    wp_view_orbit(&v, 0.0f, 30.0f);
    expect(v.plan && v.cam.ortho, "pitch in plan stays a plan (map does not become 3D)");
    expect(fabsf(v.cam.eye[0] - v.center[0]) < 1e-5f && fabsf(v.cam.eye[2] - v.center[2]) < 1e-5f,
           "pitch in plan keeps the eye above");
    expect(isfinite(v.cam.eye[0]) && isfinite(v.cam.eye[1]) && isfinite(v.cam.eye[2]) &&
               isfinite(v.cam.up[0]) && isfinite(v.cam.up[1]) && isfinite(v.cam.up[2]),
           "plan orbit does not NaN the camera");

    {
        struct wp_aabb box;
        struct wp_vn_vertex cv[24];
        uint16_t ci[36];
        uint32_t cnv = 0, cni = 0, vi;
        float yaw_keep;

        wp_cube_cpu(cv, ci, &cnv, &cni);
        expect(wp_aabb_from_vn(&box, cv, cnv) == 0, "fit uses cube AABB");
        wp_view_init(&v);
        yaw_keep = v.yaw;
        expect(wp_view_fit(&v, &box) == 0, "fit unit cube");
        expect(fabsf(v.center[0]) + fabsf(v.center[1]) + fabsf(v.center[2]) < 1e-4f,
               "fit of a centered cube keeps look-at at origin");
        expect(fabsf(v.yaw - yaw_keep) < 1e-6f, "fit keeps yaw");
        expect(v.dist > 1.0f && v.dist < 8.0f, "fit dist frames a unit cube");
        expect(v.cam.zfar > v.cam.znear + 1.0f, "fit sets a real clip range");
        expect(wp_view_fit(NULL, &box) == -EINVAL, "fit null view");
        wp_aabb_reset(&box);
        expect(wp_view_fit(&v, &box) == -EINVAL, "fit empty box");

        wp_cube_cpu(cv, ci, &cnv, &cni);
        for (vi = 0; vi < cnv; vi++)
            cv[vi].px += 4.0f;
        expect(wp_aabb_from_vn(&box, cv, cnv) == 0, "offset cube AABB");
        wp_view_init(&v);
        expect(wp_view_fit(&v, &box) == 0, "fit offset cube");
        expect(fabsf(v.center[0] - 4.0f) < 1e-4f && fabsf(v.center[1]) < 1e-4f &&
                   fabsf(v.center[2]) < 1e-4f,
               "fit look-at follows the mesh");

        wp_view_init(&v);
        wp_view_plan(&v);
        expect(wp_view_fit(&v, &box) == 0, "fit in plan");
        expect(v.plan && v.cam.ortho, "fit keeps plan+ortho");
        expect(fabsf(v.center[0] - 4.0f) < 1e-4f, "plan fit still tracks the mesh");

        box.min[0] = box.min[1] = box.min[2] = -50.0f;
        box.max[0] = box.max[1] = box.max[2] = 50.0f;
        wp_view_init(&v);
        expect(wp_view_fit(&v, &box) == 0, "fit 100-unit box");
        expect(v.dist > WP_VIEW_DIST_MAX, "fit dist is not the unit-cube cap of 18");
        expect(v.cam.zfar > WP_VIEW_DIST_MAX, "fit zfar is not 20");
        expect(v.dist_max > WP_VIEW_DIST_MAX, "dolly after fit can go past 18");
    }

    wp_view_init(&v);
    t0 = now_ns();
    for (k = 0; k < 100000; k++)
        wp_view_orbit(&v, 1.0f, 0.0f);
    t1 = now_ns();
    printf("      orbit × 100000  %.2f ns/call\n", (double)(t1 - t0) / 100000.0);
    expect(v.yaw != yaw0, "100k orbits moved yaw");
    wp_view_init(&v);
    t0 = now_ns();
    for (k = 0; k < 100000; k++)
        wp_view_pan(&v, 1.0f, 0.0f);
    t1 = now_ns();
    printf("      pan   × 100000  %.2f ns/call\n", (double)(t1 - t0) / 100000.0);
}

int main(void)
{
    struct wp_session *s;
    struct wp_present *p;
    struct wp_pass pass;
    struct wp_lit lit;
    struct wp_mesh mesh;
    struct wp_view view;
    struct wp_present_frame f;
    float model[16];
    uint8_t *pix;
    const uint8_t *c;
    unsigned presented = 0;
    uint64_t deadline, t0;
    int ret;

    cpu_tests();

    s = calloc(1, sizeof(*s));
    p = calloc(1, sizeof(*p));
    memset(&pass, 0, sizeof(pass));
    memset(&lit, 0, sizeof(lit));
    memset(&mesh, 0, sizeof(mesh));
    pix = calloc((size_t)RW * RH, 4);
    if (!s || !p || !pix) {
        free(s);
        free(p);
        free(pix);
        return 1;
    }
    wp_mat4_identity(model);
    wp_view_init(&view);
    view.rect = (struct wp_rect){ 0, 0, 64, 128 };
    view.cam.eye[0] = 0.0f;
    view.cam.eye[1] = 0.0f;
    view.cam.eye[2] = 2.5f;
    view.cam.center[0] = 0.0f;
    view.cam.center[1] = 0.0f;
    view.cam.center[2] = 0.0f;

    ret = wp_session_open(s);
    expect(ret == 0, "session_open");
    if (ret < 0)
        goto done;
    ret = wp_session_setup_surface(s);
    expect(ret == 0, "surface");
    if (ret < 0)
        goto done;
    ret = wp_present_open(p, s);
    expect(ret == 0, "present_open");
    if (ret < 0)
        goto done;
    ret = wp_pass_init(&pass, &p->device, VK_FORMAT_R8G8B8A8_UNORM, RW, RH);
    expect(ret == 0, "pass");
    if (ret < 0)
        goto done;
    ret = wp_lit_init(&lit, &p->device, VK_FORMAT_R8G8B8A8_UNORM);
    expect(ret == 0, "lit");
    if (ret < 0)
        goto done;
    ret = wp_mesh_cube(&p->device, &mesh);
    expect(ret == 0, "mesh");
    if (ret < 0)
        goto done;

    memset(pix, 0, (size_t)RW * RH * 4);
    ret = render_view(&p->device, &pass, &lit, &mesh, &view, 1, model, pix);
    expect(ret == 0, "GPU readback of a left-half view");
    if (ret == 0) {
        c = px_at(pix, 32, RH / 2);
        printf("      left-pane center RGBA %u %u %u %u\n", c[0], c[1], c[2], c[3]);
        expect(is_plus_z(c), "cube fills the view pane (viewport+scissor, not a cropped window)");
        c = px_at(pix, 96, RH / 2);
        printf("      right-half center RGBA %u %u %u %u\n", c[0], c[1], c[2], c[3]);
        expect(is_clear(c), "pixels outside the view are the clear (not a fullscreen cube)");
    }

    {
        struct wp_view pair[2];
        pair[0] = view;
        pair[1] = view;
        pair[1].rect = (struct wp_rect){ 64, 0, 64, 128 };
        memset(pix, 0, (size_t)RW * RH * 4);
        ret = render_view(&p->device, &pass, &lit, &mesh, pair, 2, model, pix);
        expect(ret == 0, "GPU readback of two panes");
        if (ret == 0) {
            c = px_at(pix, 32, RH / 2);
            expect(is_plus_z(c), "left pane of a split still +Z");
            c = px_at(pix, 96, RH / 2);
            printf("      two-view right RGBA %u %u %u %u\n", c[0], c[1], c[2], c[3]);
            expect(is_plus_z(c), "right pane is a second view (not leftover clear)");
        }
    }

    {
        struct wp_view pair[2];
        pair[0] = view;
        pair[1] = view;
        pair[1].rect = (struct wp_rect){ 64, 0, 64, 128 };
        pair[1].layers = 0;
        memset(pix, 0, (size_t)RW * RH * 4);
        ret = render_view(&p->device, &pass, &lit, &mesh, pair, 2, model, pix);
        expect(ret == 0, "GPU readback of two panes, right layers 0");
        if (ret == 0) {
            c = px_at(pix, 32, RH / 2);
            printf("      layers-off left RGBA %u %u %u %u\n", c[0], c[1], c[2], c[3]);
            expect(is_plus_z(c), "left pane still +Z with right layers 0");
            c = px_at(pix, 96, RH / 2);
            printf("      layers-off right RGBA %u %u %u %u\n", c[0], c[1], c[2], c[3]);
            expect(is_clear(c), "right pane with layers 0 is the clear");
            expect(!is_plus_z(c), "right pane is not leftover cube");
        }
    }

    {
        struct wp_view pair[2];
        wp_view_init(&pair[0]);
        pair[0].rect = (struct wp_rect){ 0, 0, 64, 128 };
        pair[0].cam.eye[0] = 0.0f;
        pair[0].cam.eye[1] = 0.0f;
        pair[0].cam.eye[2] = 2.5f;
        pair[0].cam.center[0] = 0.0f;
        pair[0].cam.center[1] = 0.0f;
        pair[0].cam.center[2] = 0.0f;
        wp_view_init(&pair[1]);
        pair[1].rect = (struct wp_rect){ 64, 0, 64, 128 };
        wp_view_plan(&pair[1]);
        memset(pix, 0, (size_t)RW * RH * 4);
        ret = render_view(&p->device, &pass, &lit, &mesh, pair, 2, model, pix);
        expect(ret == 0, "GPU readback of +Z and plan");
        if (ret == 0) {
            c = px_at(pix, 32, RH / 2);
            printf("      plan-split left RGBA %u %u %u %u\n", c[0], c[1], c[2], c[3]);
            expect(is_plus_z(c), "left pane is still +Z");
            c = px_at(pix, 96, RH / 2);
            printf("      plan-split right RGBA %u %u %u %u\n", c[0], c[1], c[2], c[3]);
            expect(is_plus_y(c), "right pane is the cube top (plan, not +Z)");
            expect(!is_plus_z(c), "plan is not a head-on +Z face");
            expect(!is_clear(c), "plan is not the clear");
        }
    }

    {
        struct wp_view planv;
        unsigned n_ortho, n_persp;

        wp_view_init(&planv);
        planv.rect = (struct wp_rect){ 0, 0, 128, 128 };
        wp_view_plan(&planv);
        memset(pix, 0, (size_t)RW * RH * 4);
        ret = render_view(&p->device, &pass, &lit, &mesh, &planv, 1, model, pix);
        expect(ret == 0, "GPU readback of ortho plan");
        n_ortho = 0;
        n_persp = 0;
        if (ret == 0) {
            c = px_at(pix, RW / 2, RH / 2);
            printf("      ortho-plan center RGBA %u %u %u %u\n", c[0], c[1], c[2], c[3]);
            expect(is_plus_y(c), "ortho plan of the cube is still +Y");
            expect(!is_plus_z(c), "ortho plan is not +Z");
            n_ortho = count_plus_y(pix);
            printf("      ortho-plan +Y pixels %u\n", n_ortho);
            expect(n_ortho > 400, "ortho plan covers a real cube top, not a speck");
        }
        planv.cam.ortho = 0;
        memset(pix, 0, (size_t)RW * RH * 4);
        ret = render_view(&p->device, &pass, &lit, &mesh, &planv, 1, model, pix);
        expect(ret == 0, "GPU readback of perspective from the plan eye");
        if (ret == 0) {
            n_persp = count_plus_y(pix);
            printf("      persp-from-plan-eye +Y pixels %u\n", n_persp);
            expect(n_persp > n_ortho + 80,
                   "ortho +Y count is less than perspective (near face not magnified)");
        }
    }

    {
        struct wp_mesh shifted;
        struct wp_vn_vertex vtx[24];
        uint16_t idx[36];
        struct wp_aabb box;
        struct wp_view fitv;
        uint32_t nv = 0, ni = 0, ti;

        memset(&shifted, 0, sizeof(shifted));
        wp_cube_cpu(vtx, idx, &nv, &ni);
        for (ti = 0; ti < nv; ti++)
            vtx[ti].px += 4.0f;
        ret = wp_mesh_upload(&p->device, &shifted, vtx, nv, idx, ni);
        expect(ret == 0, "upload cube translated +X 4");
        if (ret == 0) {
            expect(wp_aabb_from_vn(&box, vtx, nv) == 0, "AABB of translated cube");
            wp_view_init(&fitv);
            fitv.rect = (struct wp_rect){ 0, 0, 128, 128 };
            fitv.cam.eye[0] = 0.0f;
            fitv.cam.eye[1] = 0.0f;
            fitv.cam.eye[2] = 2.5f;
            fitv.cam.center[0] = 0.0f;
            fitv.cam.center[1] = 0.0f;
            fitv.cam.center[2] = 0.0f;
            memset(pix, 0, (size_t)RW * RH * 4);
            ret = render_view(&p->device, &pass, &lit, &shifted, &fitv, 1, model, pix);
            expect(ret == 0, "GPU readback of translated cube, unfitted");
            if (ret == 0) {
                c = px_at(pix, RW / 2, RH / 2);
                printf("      unfitted offset cube RGBA %u %u %u %u\n", c[0], c[1], c[2], c[3]);
                expect(is_clear(c), "unfitted offset cube is not at the origin");
                expect(!is_plus_z(c), "unfitted center is not leftover +Z");
            }
            fitv.yaw = 0.0f;
            fitv.pitch = 0.0f;
            expect(wp_view_fit(&fitv, &box) == 0, "fit translated cube");
            memset(pix, 0, (size_t)RW * RH * 4);
            ret = render_view(&p->device, &pass, &lit, &shifted, &fitv, 1, model, pix);
            expect(ret == 0, "GPU readback after fit");
            if (ret == 0) {
                c = px_at(pix, RW / 2, RH / 2);
                printf("      fitted offset cube RGBA %u %u %u %u\n", c[0], c[1], c[2], c[3]);
                expect(is_plus_z(c), "after fit the cube +Z is at center");
                expect(!is_clear(c), "fit is not the clear");
            }
            wp_mesh_destroy(&p->device, &shifted);
        }
    }

    wp_pass_destroy(&pass);
    wp_lit_destroy(&lit);
    ret = wp_pass_init(&pass, &p->device, p->negotiated.vk_format, p->sc.width, p->sc.height);
    expect(ret == 0, "pass matching swapchain");
    if (ret < 0)
        goto done;
    ret = wp_lit_init(&lit, &p->device, p->negotiated.vk_format);
    expect(ret == 0, "lit matching swapchain");
    if (ret < 0)
        goto done;

    t0 = now_ns();
    deadline = t0 + 8000000000ull;
    while (presented < 4 && now_ns() < deadline && !s->closed) {
        int32_t vx, vy;
        uint32_t vw, vh;
        ret = wp_present_poll(p, 50ull * 1000ull * 1000ull);
        if (ret < 0) {
            g_fail++;
            break;
        }
        if (!wp_present_begin(p, &f))
            continue;
        view.rect = (struct wp_rect){ 0, 0, (float)f.logical_width / 2.0f, (float)f.logical_height };
        wp_pass_opaque_begin(&pass, f.cmd, f.view, f.extent.width, f.extent.height, f.slot);
        if (wp_view_bind(&view, f.cmd, f.scale, f.extent.width, f.extent.height) == 0 &&
            wp_view_pixels(&view, f.scale, f.extent.width, f.extent.height, &vx, &vy, &vw, &vh) == 0) {
            wp_lit_reset(&lit, f.slot);
            wp_lit_draw(&lit, f.cmd, vw, vh, f.slot, &mesh, &view.cam, model);
        }
        wp_pass_opaque_end(f.cmd);
        wp_pass_overlay_begin(&pass, f.cmd, f.view, f.extent.width, f.extent.height);
        wp_pass_overlay_end(f.cmd);
        ret = wp_present_end(p, &f);
        if (ret < 0) {
            g_fail++;
            break;
        }
        presented++;
    }
    printf("      view frames %u  %ux%u\n", presented, p->sc.width, p->sc.height);
    expect(presented >= 2, "presented frames with a view pane");

done:
    if (p)
        wp_mesh_destroy(&p->device, &mesh);
    wp_lit_destroy(&lit);
    wp_pass_destroy(&pass);
    wp_present_close(p);
    wp_session_close(s);
    free(p);
    free(s);
    free(pix);
    printf("%s  %d failure(s)\n", g_fail ? "RESULT FAIL" : "RESULT PASS", g_fail);
    return g_fail ? 1 : 0;
}

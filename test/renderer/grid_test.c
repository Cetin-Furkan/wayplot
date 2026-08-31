#define _GNU_SOURCE
#include "engine/present.h"
#include "engine/view.h"
#include "helper/math3d.h"
#include "renderer/camera.h"
#include "renderer/grid.h"
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

static int is_grid(const uint8_t *p)
{
    int dr = abs((int)p[0] - (int)p[1]);
    int db = abs((int)p[1] - (int)p[2]);

    return p[0] > 45 && p[1] > 45 && p[2] > 45 && dr < 45 && db < 50 && !is_plus_z(p) &&
           !is_clear(p);
}

static unsigned count_pred(const uint8_t *pix, int (*pred)(const uint8_t *))
{
    unsigned n = 0;
    int i;

    for (i = 0; i < RW * RH; i++) {
        if (pred(pix + (size_t)i * 4))
            n++;
    }
    return n;
}

static int nice_125(float step)
{
    float expn, f;

    if (!(step > 0.0f) || !isfinite(step))
        return 0;
    expn = powf(10.0f, floorf(log10f(step)));
    f = step / expn;
    return fabsf(f - 1.0f) < 1e-4f || fabsf(f - 2.0f) < 1e-4f || fabsf(f - 5.0f) < 1e-4f;
}

static void cpu_tests(void)
{
    struct wp_aabb box;
    struct wp_grid g;
    struct wp_mesh_cpu cpu;
    struct wp_vn_vertex vtx[24];
    uint16_t idx[36];
    uint32_t nv = 0, ni = 0, i, nlines;
    struct wp_camera cam;
    float vp[16], a[3], b[3], c[3];
    int ret, k;
    uint64_t t0, t1;

    memset(&g, 0, sizeof(g));
    memset(&cpu, 0, sizeof(cpu));
    wp_aabb_reset(&box);
    expect(wp_grid_from_aabb(NULL, &g) == -EINVAL, "null box");
    expect(wp_grid_from_aabb(&box, &g) == -EINVAL, "empty box");

    wp_cube_cpu(vtx, idx, &nv, &ni);
    expect(wp_aabb_from_vn(&box, vtx, nv) == 0, "cube AABB");
    ret = wp_grid_from_aabb(&box, &g);
    expect(ret == 0, "grid from unit cube");
    expect(nice_125(g.step), "step is 1, 2, or 5 times a power of 10");
    expect(g.ix0 <= 0 && g.ix1 >= 0 && g.iz0 <= 0 && g.iz1 >= 0, "grid is origin-aligned");
    expect(g.y < box.min[1] - 1e-4f, "grid sits under the AABB");
    nlines = (uint32_t)(g.ix1 - g.ix0 + 1 + g.iz1 - g.iz0 + 1);
    expect(nlines >= 8 && nlines <= WP_GRID_MAX_LINES, "a real lattice, not one line");

    ret = wp_grid_tessellate(&g, &cpu);
    expect(ret == 0, "tessellate");
    expect(cpu.nv == nlines * 4u && cpu.ni == nlines * 6u, "4 verts / 6 indices per line");
    wp_camera_default(&cam);
    cam.eye[0] = 0.0f;
    cam.eye[1] = 2.5f;
    cam.eye[2] = 0.0f;
    cam.center[0] = 0.0f;
    cam.center[1] = 0.0f;
    cam.center[2] = 0.0f;
    cam.up[0] = 0.0f;
    cam.up[1] = 0.0f;
    cam.up[2] = 1.0f;
    wp_camera_vp(&cam, 1.0f, vp);
    a[0] = cpu.v[cpu.idx[0]].px;
    a[1] = cpu.v[cpu.idx[0]].py;
    a[2] = cpu.v[cpu.idx[0]].pz;
    b[0] = cpu.v[cpu.idx[1]].px;
    b[1] = cpu.v[cpu.idx[1]].py;
    b[2] = cpu.v[cpu.idx[1]].pz;
    c[0] = cpu.v[cpu.idx[2]].px;
    c[1] = cpu.v[cpu.idx[2]].py;
    c[2] = cpu.v[cpu.idx[2]].pz;
    expect(wp_triangle_front_facing(vp, a, b, c), "first grid tri is a raster front from +Y");
    {
        int same_y = 1, up = 1;
        for (i = 0; i < cpu.nv; i++) {
            if (fabsf(cpu.v[i].py - cpu.v[0].py) > 1e-5f)
                same_y = 0;
            if (cpu.v[i].ny < 0.9f)
                up = 0;
        }
        expect(same_y, "all grid verts share Y (an XZ plane)");
        expect(up, "grid normals are +Y");
    }
    t0 = now_ns();
    for (k = 0; k < 400; k++) {
        struct wp_mesh_cpu m;
        memset(&m, 0, sizeof(m));
        if (wp_grid_tessellate(&g, &m) == 0)
            wp_mesh_cpu_free(&m);
    }
    t1 = now_ns();
    printf("      tessellate %u lines × 400  %.2f ns/line\n", nlines,
           (double)(t1 - t0) / (400.0 * (double)nlines));
    wp_mesh_cpu_free(&cpu);
}

static int render_to(struct wp_device *d, struct wp_pass *pass, struct wp_lit *lit,
                     struct wp_mesh *a, struct wp_mesh *b, const struct wp_camera *cam,
                     const float model[16], uint8_t *pixels)
{
    VkImage img = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
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
        .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1,
                              .layerCount = 1 },
    };
    VkCommandBufferAllocateInfo cai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
        .commandPool = d->pool,
    };
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VkImageMemoryBarrier2 b1 = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1,
                              .layerCount = 1 },
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
        .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1,
                              .layerCount = 1 },
    };
    VkDependencyInfo dep = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                             .imageMemoryBarrierCount = 1 };
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkImageToMemoryCopy region = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_TO_MEMORY_COPY,
        .pHostPointer = pixels,
        .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .imageExtent = { RW, RH, 1 },
    };
    VkCopyImageToMemoryInfo cpy = { .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_MEMORY_INFO,
                                    .srcImageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .regionCount = 1,
                                    .pRegions = &region };
    uint32_t midx;
    int ret = -EIO;

    if (!d->host_image_copy)
        return -ENOTSUP;
    vkGetDeviceImageMemoryRequirements(d->device, &q, &memreq);
    if (vkCreateImage(d->device, &ici, NULL, &img) != VK_SUCCESS)
        return -EIO;
    vkGetPhysicalDeviceMemoryProperties(d->phy, &mprops);
    midx = wp_find_memory_type(&mprops, memreq.memoryRequirements.memoryTypeBits,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (midx == UINT32_MAX)
        midx = wp_find_memory_type(&mprops, memreq.memoryRequirements.memoryTypeBits, 0);
    if (midx == UINT32_MAX)
        goto out;
    ai.allocationSize = memreq.memoryRequirements.size;
    ai.memoryTypeIndex = midx;
    if (vkAllocateMemory(d->device, &ai, NULL, &mem) != VK_SUCCESS)
        goto out;
    if (vkBindImageMemory(d->device, img, mem, 0) != VK_SUCCESS)
        goto out;
    vci.image = img;
    if (vkCreateImageView(d->device, &vci, NULL, &view) != VK_SUCCESS)
        goto out;
    if (vkAllocateCommandBuffers(d->device, &cai, &cmd) != VK_SUCCESS)
        goto out;
    vkBeginCommandBuffer(cmd, &bi);
    b1.image = img;
    dep.pImageMemoryBarriers = &b1;
    vkCmdPipelineBarrier2(cmd, &dep);
    wp_pass_opaque_begin(pass, cmd, view, RW, RH, 0);
    wp_lit_reset(lit, 0);
    if (a && a->index_count)
        wp_lit_draw(lit, cmd, RW, RH, 0, a, cam, model);
    if (b && b->index_count)
        wp_lit_draw(lit, cmd, RW, RH, 0, b, cam, model);
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
    if (view)
        vkDestroyImageView(d->device, view, NULL);
    if (img)
        vkDestroyImage(d->device, img, NULL);
    if (mem)
        vkFreeMemory(d->device, mem, NULL);
    return ret;
}

int main(void)
{
    struct wp_session *s;
    struct wp_present *p;
    struct wp_pass pass;
    struct wp_lit lit;
    struct wp_mesh grid_m, cube_m;
    struct wp_grid g;
    struct wp_mesh_cpu cpu;
    struct wp_aabb box, tiny;
    struct wp_view view;
    struct wp_present_frame f;
    struct wp_vn_vertex vtx[24];
    uint16_t idx[36];
    float model[16];
    uint8_t *pix;
    const uint8_t *c;
    unsigned presented = 0, n_grid, n_clear;
    uint64_t deadline, t0;
    uint32_t nv = 0, ni = 0;
    int ret;

    cpu_tests();

    s = calloc(1, sizeof(*s));
    p = calloc(1, sizeof(*p));
    memset(&pass, 0, sizeof(pass));
    memset(&lit, 0, sizeof(lit));
    memset(&grid_m, 0, sizeof(grid_m));
    memset(&cube_m, 0, sizeof(cube_m));
    memset(&cpu, 0, sizeof(cpu));
    pix = calloc((size_t)RW * RH, 4);
    if (!s || !p || !pix) {
        free(s);
        free(p);
        free(pix);
        return 1;
    }
    wp_mat4_identity(model);
    wp_cube_cpu(vtx, idx, &nv, &ni);
    expect(wp_aabb_from_vn(&box, vtx, nv) == 0, "GPU cube AABB");
    expect(wp_grid_from_aabb(&box, &g) == 0, "GPU grid from cube");

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
    ret = wp_grid_tessellate(&g, &cpu);
    expect(ret == 0, "tessellate for GPU");
    if (ret == 0) {
        ret = wp_mesh_upload(&p->device, &grid_m, cpu.v, cpu.nv, cpu.idx, cpu.ni);
        expect(ret == 0, "upload grid");
    }
    wp_mesh_cpu_free(&cpu);
    if (ret < 0)
        goto done;
    ret = wp_mesh_cube(&p->device, &cube_m);
    expect(ret == 0, "upload cube");
    if (ret < 0)
        goto done;

    wp_view_init(&view);
    view.rect = (struct wp_rect){ 0, 0, 128, 128 };
    wp_view_plan(&view);
    tiny.min[0] = tiny.min[1] = tiny.min[2] = -0.12f;
    tiny.max[0] = tiny.max[1] = tiny.max[2] = 0.12f;
    expect(wp_view_fit(&view, &tiny) == 0, "close plan around the origin");

    memset(pix, 0, (size_t)RW * RH * 4);
    ret = render_to(&p->device, &pass, &lit, &grid_m, NULL, &view.cam, model, pix);
    expect(ret == 0, "GPU readback of grid in plan");
    if (ret == 0) {
        c = px_at(pix, RW / 2, RH / 2);
        printf("      plan-grid center RGBA %u %u %u %u\n", c[0], c[1], c[2], c[3]);
        expect(is_grid(c), "close plan hits the origin crossing");
        expect(!is_clear(c) && !is_plus_z(c), "origin is not the clear and not +Z");
        n_grid = count_pred(pix, is_grid);
        n_clear = count_pred(pix, is_clear);
        printf("      plan-grid pixels %u  clear %u\n", n_grid, n_clear);
        expect(n_grid > 80, "grid left real pixels (not sub-pixel nothing)");
        expect(n_clear > n_grid, "most of the pane is still the clear (lattice, not a floor)");
    }

    {
        float pan_s, dpx;

        pan_s = view.dist * WP_VIEW_PAN;
        dpx = (0.5f * g.step) / (pan_s > 1e-8f ? pan_s : 1e-8f);
        /* Diagonal: axis-aligned pan stays on the origin line. */
        wp_view_pan(&view, dpx, dpx);
    }
    memset(pix, 0, (size_t)RW * RH * 4);
    ret = render_to(&p->device, &pass, &lit, &grid_m, NULL, &view.cam, model, pix);
    expect(ret == 0, "GPU readback after plan pan");
    if (ret == 0) {
        c = px_at(pix, RW / 2, RH / 2);
        printf("      panned-grid center RGBA %u %u %u %u\n", c[0], c[1], c[2], c[3]);
        expect(is_clear(c), "half-cell pan puts the center in a cell (not overlay lines)");
        expect(!is_grid(c), "panned center is not still the origin cross");
    }

    wp_view_init(&view);
    view.rect = (struct wp_rect){ 0, 0, 128, 128 };
    wp_view_plan(&view);
    expect(wp_view_fit(&view, &tiny) == 0, "refit close plan");
    memset(pix, 0, (size_t)RW * RH * 4);
    ret = render_to(&p->device, &pass, &lit, NULL, NULL, &view.cam, model, pix);
    expect(ret == 0, "GPU readback with grid skipped");
    if (ret == 0) {
        c = px_at(pix, RW / 2, RH / 2);
        printf("      skipped-grid center RGBA %u %u %u %u\n", c[0], c[1], c[2], c[3]);
        expect(is_clear(c), "skipping the mesh is the clear");
        expect(count_pred(pix, is_grid) == 0, "no leftover grid pixels without a draw");
    }

    {
        struct wp_view head;

        wp_view_init(&head);
        head.rect = (struct wp_rect){ 0, 0, 128, 128 };
        head.yaw = 0.0f;
        head.pitch = 0.0f;
        head.dist = 2.5f;
        head.center[0] = head.center[1] = head.center[2] = 0.0f;
        wp_view_orbit(&head, 0.0f, 0.0f);
        head.cam.eye[0] = 0.0f;
        head.cam.eye[1] = 0.0f;
        head.cam.eye[2] = 2.5f;
        head.cam.center[0] = 0.0f;
        head.cam.center[1] = 0.0f;
        head.cam.center[2] = 0.0f;
        head.cam.up[0] = 0.0f;
        head.cam.up[1] = 1.0f;
        head.cam.up[2] = 0.0f;
        memset(pix, 0, (size_t)RW * RH * 4);
        ret = render_to(&p->device, &pass, &lit, &cube_m, &grid_m, &head.cam, model, pix);
        expect(ret == 0, "GPU readback of cube + grid");
        if (ret == 0) {
            c = px_at(pix, RW / 2, RH / 2);
            printf("      cube+grid center RGBA %u %u %u %u\n", c[0], c[1], c[2], c[3]);
            expect(is_plus_z(c), "head-on +Z is still the cube, not a grid floor");
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
    wp_view_init(&view);
    wp_view_plan(&view);
    expect(wp_view_fit(&view, &box) == 0, "present fit of the cube box");
    while (presented < 4 && now_ns() < deadline && !s->closed) {
        ret = wp_present_poll(p, 50ull * 1000ull * 1000ull);
        if (ret < 0) {
            g_fail++;
            break;
        }
        if (!wp_present_begin(p, &f))
            continue;
        wp_pass_opaque_begin(&pass, f.cmd, f.view, f.extent.width, f.extent.height, f.slot);
        wp_lit_reset(&lit, f.slot);
        wp_lit_draw(&lit, f.cmd, f.extent.width, f.extent.height, f.slot, &grid_m, &view.cam, model);
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
    printf("      grid frames %u  %ux%u\n", presented, p->sc.width, p->sc.height);
    expect(presented >= 2, "presented frames with the grid mesh");

done:
    if (p) {
        wp_mesh_destroy(&p->device, &grid_m);
        wp_mesh_destroy(&p->device, &cube_m);
    }
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

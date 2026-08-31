#define _GNU_SOURCE
#include "engine/doc.h"
#include "engine/present.h"
#include "engine/view.h"
#include "helper/math3d.h"
#include "renderer/camera.h"
#include "renderer/dem.h"
#include "renderer/image.h"
#include "renderer/lit.h"
#include "renderer/mesh.h"
#include "renderer/pass.h"

#include <errno.h>
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

static int is_plus_y(const uint8_t *p)
{
    return p[1] > 100 && p[2] > 100 && (int)p[1] > (int)p[0] + 40 &&
           (int)p[2] > (int)p[0] + 40;
}

static int is_red(const uint8_t *p)
{
    return p[0] > 120 && (int)p[0] > (int)p[1] + 60 && (int)p[0] > (int)p[2] + 60;
}

static int is_green(const uint8_t *p)
{
    return p[1] > 120 && (int)p[1] > (int)p[0] + 60 && (int)p[1] > (int)p[2] + 60;
}

static int is_dem(const uint8_t *p)
{
    return p[1] > 40 && (int)p[1] >= (int)p[0] - 8 && !is_clear(p) && !is_red(p) && !is_green(p);
}

static void fill_split_ppm(unsigned char *buf, size_t *len)
{
    uint32_t i, j, o;
    memcpy(buf, "P6\n32 2\n255\n", 12);
    o = 12;
    for (j = 0; j < 2; j++) {
        for (i = 0; i < 32; i++) {
            if (i < 16) {
                buf[o++] = 255;
                buf[o++] = 0;
                buf[o++] = 0;
            } else {
                buf[o++] = 0;
                buf[o++] = 255;
                buf[o++] = 0;
            }
        }
    }
    *len = o;
}

static void cpu_tests(void)
{
    struct wp_image im;
    struct wp_mesh_cpu cpu;
    struct wp_camera cam;
    float vp[16], a[3], b[3], c[3];
    unsigned char ppm[12 + 32 * 2 * 3];
    size_t plen;
    int ret;

    memset(&im, 0, sizeof(im));
    expect(wp_image_load("/no/such/wayplot-image.ppm", &im) == -ENOENT, "missing file is ENOENT");
    expect(wp_image_parse_ppm("P5\n", 3, &im) == -EINVAL, "P5 is not P6");
    fill_split_ppm(ppm, &plen);
    ret = wp_image_parse_ppm(ppm, plen, &im);
    expect(ret == 0 && im.w == 32 && im.h == 2, "32x2 P6");
    expect(im.rgba[0] == 255 && im.rgba[1] == 0 && im.rgba[3] == 255, "left is red RGBA");
    expect(im.rgba[16 * 4] == 0 && im.rgba[16 * 4 + 1] == 255, "right is green");
    wp_image_free(&im);

    ret = wp_image_quad(&cpu);
    expect(ret == 0 && cpu.nv == 4 && cpu.ni == 6, "quad is 4 verts / 6 indices");
    expect(cpu.v[0].u == 0.0f && cpu.v[1].u == 1.0f && cpu.v[3].v == 1.0f, "UVs span 0..1");
    wp_camera_default(&cam);
    cam.eye[0] = 0.0f;
    cam.eye[1] = 2.5f;
    cam.eye[2] = 0.2f;
    cam.up[0] = 0.0f;
    cam.up[1] = 0.0f;
    cam.up[2] = -1.0f;
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
    expect(wp_triangle_front_facing(vp, a, b, c), "quad first tri is a raster front from +Y");
    wp_mesh_cpu_free(&cpu);

    {
        struct wp_aabb box;
        struct wp_vn_vertex vtx[24];
        uint16_t idx[36];
        uint32_t nv = 0, ni = 0;

        wp_cube_cpu(vtx, idx, &nv, &ni);
        expect(wp_aabb_from_vn(&box, vtx, nv) == 0, "cube AABB for ground");
        expect(wp_image_ground(NULL, &cpu) == -EINVAL, "ground null box");
        ret = wp_image_ground(&box, &cpu);
        expect(ret == 0 && cpu.nv == 4 && cpu.ni == 12, "ground is a quad, both sides");
        expect(cpu.v[0].py < box.min[1] - 1e-4f, "ground sits under the cube");
        expect(cpu.v[0].px <= box.min[0] && cpu.v[1].px >= box.max[0], "ground covers cube X");
        expect(cpu.v[0].u == 0.0f && cpu.v[1].u == 1.0f && cpu.v[3].v == 1.0f, "ground UVs span 0..1");
        wp_mesh_cpu_free(&cpu);
    }
}

static int render_to(struct wp_device *d, struct wp_pass *pass, struct wp_lit *lit,
                     struct wp_mesh *mesh, const struct wp_camera *cam, const float model[16],
                     const struct wp_tex *tex, struct wp_mesh *mesh2, const struct wp_tex *tex2,
                     uint8_t *pixels)
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
    uint32_t idx;
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
    if (mesh && mesh->index_count)
        wp_lit_draw_tex(lit, cmd, RW, RH, 0, mesh, cam, model, tex);
    if (mesh2 && mesh2->index_count)
        wp_lit_draw_tex(lit, cmd, RW, RH, 0, mesh2, cam, model, tex2);
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
    struct wp_mesh quad, cube, height;
    struct wp_image im;
    struct wp_tex tex;
    struct wp_doc doc;
    struct wp_mesh_cpu cpu;
    struct wp_camera cam, cube_cam;
    struct wp_present_frame f;
    float model[16];
    uint8_t *pix;
    const uint8_t *c0, *c1;
    unsigned char ppm[12 + 32 * 2 * 3];
    size_t plen;
    unsigned presented = 0;
    uint64_t deadline, t0, t1;
    int ret;

    cpu_tests();

    s = calloc(1, sizeof(*s));
    p = calloc(1, sizeof(*p));
    memset(&pass, 0, sizeof(pass));
    memset(&lit, 0, sizeof(lit));
    memset(&quad, 0, sizeof(quad));
    memset(&cube, 0, sizeof(cube));
    memset(&height, 0, sizeof(height));
    memset(&im, 0, sizeof(im));
    memset(&tex, 0, sizeof(tex));
    memset(&cpu, 0, sizeof(cpu));
    wp_doc_init(&doc);
    pix = calloc((size_t)RW * RH, 4);
    if (!s || !p || !pix) {
        free(s);
        free(p);
        free(pix);
        return 1;
    }
    wp_mat4_identity(model);
    wp_camera_default(&cam);
    cam.eye[0] = 0.0f;
    cam.eye[1] = 2.5f;
    cam.eye[2] = 0.2f;
    cam.center[0] = 0.0f;
    cam.center[1] = 0.0f;
    cam.center[2] = 0.0f;
    cam.up[0] = 0.0f;
    cam.up[1] = 0.0f;
    cam.up[2] = -1.0f;
    wp_camera_default(&cube_cam);
    cube_cam.eye[0] = 0.0f;
    cube_cam.eye[1] = 0.0f;
    cube_cam.eye[2] = 2.5f;
    cube_cam.center[0] = 0.0f;
    cube_cam.center[1] = 0.0f;
    cube_cam.center[2] = 0.0f;

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

    fill_split_ppm(ppm, &plen);
    ret = wp_image_parse_ppm(ppm, plen, &im);
    expect(ret == 0, "parse split PPM");
    {
        struct wp_image big;
        memset(&big, 0, sizeof(big));
        big.w = big.h = 256;
        big.rgba = calloc(256u * 256u, 4u);
        expect(big.rgba != NULL, "256^2 pixels");
        if (big.rgba) {
            t0 = now_ns();
            ret = wp_tex_upload(&p->device, &tex, &big);
            t1 = now_ns();
            expect(ret == 0, "host-copy 256^2");
            printf("      host-copy 256x256  %.2f ms\n", (double)(t1 - t0) / 1e6);
            wp_tex_destroy(&tex);
            wp_image_free(&big);
        }
    }
    ret = wp_tex_upload(&p->device, &tex, &im);
    expect(ret == 0, "upload split tex");
    ret = wp_image_quad(&cpu);
    expect(ret == 0, "quad cpu");
    ret = wp_mesh_upload(&p->device, &quad, cpu.v, cpu.nv, cpu.idx, cpu.ni);
    expect(ret == 0, "upload quad");
    wp_mesh_cpu_free(&cpu);
    expect(wp_doc_add_mesh(&doc, &quad) == 0, "doc mesh");
    expect(wp_doc_set_albedo(&doc, 0, &tex) == 0, "doc albedo");
    expect(wp_doc_albedo(&doc, 0) == &tex, "doc albedo pointer");

    memset(pix, 0, (size_t)RW * RH * 4);
    ret = render_to(&p->device, &pass, &lit, wp_doc_mesh(&doc, 0), &cam, model, wp_doc_albedo(&doc, 0),
                    NULL, NULL, pix);
    expect(ret == 0, "GPU readback of textured quad");
    if (ret == 0) {
        {
            unsigned nr = 0, ng = 0;
            int x, y;
            for (y = 0; y < RH; y++) {
                for (x = 0; x < RW; x++) {
                    const uint8_t *px = px_at(pix, x, y);
                    if (is_red(px))
                        nr++;
                    if (is_green(px))
                        ng++;
                }
            }
            c0 = px_at(pix, 32, RH / 2);
            c1 = px_at(pix, 96, RH / 2);
            printf("      half A RGBA %u %u %u %u  half B RGBA %u %u %u %u  red %u green %u\n",
                   c0[0], c0[1], c0[2], c0[3], c1[0], c1[1], c1[2], c1[3], nr, ng);
            expect(!is_clear(c0) && !is_clear(c1), "quad is not the clear");
            expect(nr > 200 && ng > 200, "quad shows both red and green (UVs, not a 1x1 sample)");
        }
    }

    ret = wp_mesh_cube(&p->device, &cube);
    expect(ret == 0, "cube");
    memset(pix, 0, (size_t)RW * RH * 4);
    ret = render_to(&p->device, &pass, &lit, &cube, &cube_cam, model, NULL, NULL, NULL, pix);
    expect(ret == 0, "GPU readback of cube with default albedo");
    if (ret == 0) {
        c0 = px_at(pix, RW / 2, RH / 2);
        printf("      cube default albedo RGBA %u %u %u %u\n", c0[0], c0[1], c0[2], c0[3]);
        expect(is_plus_z(c0), "1x1 white albedo still leaves +Z vertex color");
    }

    {
        struct wp_aabb box;
        struct wp_mesh_cpu gcpu;
        struct wp_mesh ground;
        struct wp_view planv;
        struct wp_vn_vertex vtx[24];
        uint16_t idx[36];
        uint32_t nv = 0, ni = 0;

        memset(&ground, 0, sizeof(ground));
        memset(&gcpu, 0, sizeof(gcpu));
        wp_cube_cpu(vtx, idx, &nv, &ni);
        expect(wp_aabb_from_vn(&box, vtx, nv) == 0, "GPU cube AABB");
        ret = wp_image_ground(&box, &gcpu);
        expect(ret == 0, "ground under cube");
        ret = wp_mesh_upload(&p->device, &ground, gcpu.v, gcpu.nv, gcpu.idx, gcpu.ni);
        expect(ret == 0, "upload ground");
        wp_mesh_cpu_free(&gcpu);
        wp_view_init(&planv);
        planv.rect = (struct wp_rect){ 0, 0, 128, 128 };
        wp_view_plan(&planv);
        expect(wp_view_fit(&planv, &box) == 0, "plan fit on the cube");
        memset(pix, 0, (size_t)RW * RH * 4);
        ret = render_to(&p->device, &pass, &lit, &cube, &planv.cam, model, NULL, &ground, &tex, pix);
        expect(ret == 0, "GPU readback of cube + ground in plan");
        if (ret == 0) {
            c0 = px_at(pix, RW / 2, RH / 2);
            printf("      cube+ground plan RGBA %u %u %u %u\n", c0[0], c0[1], c0[2], c0[3]);
            expect(is_plus_y(c0), "plan center is the cube top, not the photo");
            expect(!is_red(c0) && !is_green(c0), "split does not eat the cube in plan");
        }
        memset(pix, 0, (size_t)RW * RH * 4);
        ret = render_to(&p->device, &pass, &lit, &cube, &cube_cam, model, NULL, &ground, &tex, pix);
        expect(ret == 0, "GPU readback of cube + ground head-on");
        if (ret == 0) {
            c0 = px_at(pix, RW / 2, RH / 2);
            printf("      cube+ground +Z RGBA %u %u %u %u\n", c0[0], c0[1], c0[2], c0[3]);
            expect(is_plus_z(c0), "head-on +Z is still the cube, not a photo slab");
        }
        wp_mesh_destroy(&p->device, &ground);
    }

    {
        struct wp_dem dem;
        uint32_t vi;
        memset(&dem, 0, sizeof(dem));
        dem.cols = dem.rows = 4;
        dem.h = calloc(16, sizeof(float));
        expect(dem.h != NULL, "4x4 DEM samples");
        if (dem.h) {
            for (vi = 0; vi < 16; vi++)
                dem.h[vi] = 1.0f;
            ret = wp_dem_tessellate(&dem, WP_DEM_AMP, &cpu);
            expect(ret == 0 && cpu.v[0].u == 0.0f && cpu.v[15].u == 1.0f, "DEM planar UVs");
            ret = wp_mesh_upload(&p->device, &height, cpu.v, cpu.nv, cpu.idx, cpu.ni);
            expect(ret == 0, "upload DEM");
            memset(pix, 0, (size_t)RW * RH * 4);
            ret = render_to(&p->device, &pass, &lit, &height, &cam, model, NULL, NULL, NULL, pix);
            expect(ret == 0, "GPU readback of DEM without image");
            if (ret == 0) {
                c0 = px_at(pix, RW / 2, RH / 2);
                printf("      undraped DEM RGBA %u %u %u %u\n", c0[0], c0[1], c0[2], c0[3]);
                expect(is_dem(c0), "undraped DEM is height color, not the split");
            }
            for (vi = 0; vi < cpu.nv; vi++) {
                cpu.v[vi].r = 1.0f;
                cpu.v[vi].g = 1.0f;
                cpu.v[vi].b = 1.0f;
            }
            wp_mesh_destroy(&p->device, &height);
            ret = wp_mesh_upload(&p->device, &height, cpu.v, cpu.nv, cpu.idx, cpu.ni);
            expect(ret == 0, "re-upload DEM with white verts");
            wp_mesh_cpu_free(&cpu);
            wp_dem_free(&dem);
            memset(pix, 0, (size_t)RW * RH * 4);
            ret = render_to(&p->device, &pass, &lit, &height, &cam, model, &tex, NULL, NULL, pix);
            expect(ret == 0, "GPU readback of DEM with split image");
            if (ret == 0) {
                unsigned nr = 0, ng = 0;
                int x, y;
                for (y = 0; y < RH; y++) {
                    for (x = 0; x < RW; x++) {
                        const uint8_t *px = px_at(pix, x, y);
                        if (is_red(px))
                            nr++;
                        if (is_green(px))
                            ng++;
                    }
                }
                printf("      draped DEM  red %u green %u\n", nr, ng);
                expect(nr > 200 && ng > 200, "drape shows both halves on the heightfield");
            }
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
        ret = wp_present_poll(p, 50ull * 1000ull * 1000ull);
        if (ret < 0) {
            g_fail++;
            break;
        }
        if (!wp_present_begin(p, &f))
            continue;
        wp_pass_opaque_begin(&pass, f.cmd, f.view, f.extent.width, f.extent.height, f.slot);
        wp_lit_reset(&lit, f.slot);
        wp_lit_draw_tex(&lit, f.cmd, f.extent.width, f.extent.height, f.slot, &quad, &cam, model,
                        &tex);
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
    printf("      image frames %u  %ux%u\n", presented, p->sc.width, p->sc.height);
    expect(presented >= 2, "presented frames with the textured quad");

done:
    wp_doc_destroy(&doc);
    wp_image_free(&im);
    wp_mesh_cpu_free(&cpu);
    if (p) {
        wp_tex_destroy(&tex);
        wp_mesh_destroy(&p->device, &quad);
        wp_mesh_destroy(&p->device, &cube);
        wp_mesh_destroy(&p->device, &height);
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

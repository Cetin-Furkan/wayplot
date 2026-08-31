#define _GNU_SOURCE
#include "engine/present.h"
#include "helper/math3d.h"
#include "renderer/camera.h"
#include "renderer/dem.h"
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

static int is_dem(const uint8_t *p)
{
    return p[1] > 40 && (int)p[1] >= (int)p[0] && !is_clear(p);
}

static void cpu_tests(void)
{
    struct wp_dem dem;
    struct wp_mesh_cpu cpu;
    struct wp_camera cam;
    float vp[16], a[3], b[3], c[3];
    unsigned char pgm2[] = { 'P', '5', '\n', '2', ' ', '2', '\n', '2', '5', '5', '\n', 0, 80, 160,
                             255 };
    int ret;
    uint32_t i;

    expect(wp_dem_load("/no/such/wayplot-dem.pgm", &dem) == -ENOENT, "missing file is ENOENT");
    ret = wp_dem_parse_pgm("P2\n", 3, &dem);
    expect(ret == -EINVAL, "P2 is not P5");
    ret = wp_dem_parse_pgm(pgm2, sizeof(pgm2), &dem);
    expect(ret == 0 && dem.cols == 2 && dem.rows == 2, "2x2 P5");
    expect(dem.h[0] == 0.0f && dem.h[3] > 0.99f, "samples mapped 0..1");
    ret = wp_dem_tessellate(&dem, WP_DEM_AMP, &cpu);
    expect(ret == 0 && cpu.nv == 4 && cpu.ni == 6, "2x2 tessellates to one quad");
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
    expect(wp_triangle_front_facing(vp, a, b, c), "first DEM tri is a raster front from +Y");
    expect(cpu.v[0].ny > 0.5f, "normals point +Y on a low-slope grid");
    expect(cpu.v[0].u == 0.0f && cpu.v[0].v == 0.0f, "uv origin");
    expect(cpu.v[3].u == 1.0f && cpu.v[3].v == 1.0f, "uv far corner");
    wp_mesh_cpu_free(&cpu);
    wp_dem_free(&dem);

    {
        struct wp_dem g;
        struct wp_mesh_cpu m;
        uint64_t t0, t1;
        memset(&g, 0, sizeof(g));
        g.cols = g.rows = 128;
        g.h = calloc(128u * 128u, sizeof(float));
        expect(g.h != NULL, "128^2 samples");
        if (g.h) {
            for (i = 0; i < 128u * 128u; i++)
                g.h[i] = (float)(i % 128) / 127.0f;
            t0 = now_ns();
            ret = wp_dem_tessellate(&g, 0.5f, &m);
            t1 = now_ns();
            expect(ret == 0 && m.nv == 128u * 128u, "tessellate 128^2");
            printf("      tessellate 128x128  %u verts  %.2f ns/vert\n", m.nv,
                   (double)(t1 - t0) / (double)m.nv);
            wp_mesh_cpu_free(&m);
        }
        wp_dem_free(&g);
    }
}

static int render_to(struct wp_device *d, struct wp_pass *pass, struct wp_lit *lit,
                     struct wp_mesh *mesh, const struct wp_camera *cam, const float model[16],
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
    wp_lit_draw(lit, cmd, RW, RH, 0, mesh, cam, model);
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
    struct wp_mesh mesh;
    struct wp_dem dem;
    struct wp_mesh_cpu cpu;
    struct wp_camera cam;
    struct wp_present_frame f;
    float model[16];
    uint8_t *pix;
    const uint8_t *c;
    unsigned presented = 0;
    uint64_t deadline, t0;
    int ret;
    unsigned char ramp[11 + 16];
    uint32_t i;

    cpu_tests();

    memcpy(ramp, "P5\n4 4\n255\n", 11);
    for (i = 0; i < 16; i++)
        ramp[11 + i] = (unsigned char)(i * 16);

    s = calloc(1, sizeof(*s));
    p = calloc(1, sizeof(*p));
    memset(&pass, 0, sizeof(pass));
    memset(&lit, 0, sizeof(lit));
    memset(&mesh, 0, sizeof(mesh));
    memset(&dem, 0, sizeof(dem));
    memset(&cpu, 0, sizeof(cpu));
    pix = calloc((size_t)RW * RH, 4);
    if (!s || !p || !pix) {
        free(s);
        free(p);
        free(pix);
        return 1;
    }
    wp_mat4_identity(model);
    wp_camera_default(&cam);

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

    ret = wp_dem_parse_pgm(ramp, sizeof(ramp), &dem);
    expect(ret == 0, "parse 4x4 ramp PGM");
    ret = wp_dem_tessellate(&dem, WP_DEM_AMP, &cpu);
    expect(ret == 0, "tessellate ramp");
    ret = wp_mesh_upload(&p->device, &mesh, cpu.v, cpu.nv, cpu.idx, cpu.ni);
    expect(ret == 0, "upload DEM mesh");
    wp_mesh_cpu_free(&cpu);
    memset(pix, 0, (size_t)RW * RH * 4);
    ret = render_to(&p->device, &pass, &lit, &mesh, &cam, model, pix);
    expect(ret == 0, "GPU readback of DEM");
    if (ret == 0) {
        c = px_at(pix, RW / 2, RH / 2);
        printf("      DEM center RGBA %u %u %u %u\n", c[0], c[1], c[2], c[3]);
        expect(!is_clear(c), "DEM is not an empty window");
        expect(!is_plus_z(c), "DEM is not the cube +Z red factory");
        expect(is_dem(c), "DEM center is height-colored (green-gray)");
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
        wp_lit_draw(&lit, f.cmd, f.extent.width, f.extent.height, f.slot, &mesh, &cam, model);
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
    printf("      dem frames %u  %ux%u\n", presented, p->sc.width, p->sc.height);
    expect(presented >= 2, "presented frames of a DEM mesh");

done:
    wp_mesh_cpu_free(&cpu);
    wp_dem_free(&dem);
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

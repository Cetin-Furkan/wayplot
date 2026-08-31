#define _GNU_SOURCE
#include "engine/present.h"
#include "helper/math3d.h"
#include "renderer/camera.h"
#include "renderer/lit.h"
#include "renderer/mesh.h"
#include "renderer/obj.h"
#include "renderer/pass.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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

static const char *k_cube =
    "v -0.5 -0.5  0.5\n"
    "v  0.5 -0.5  0.5\n"
    "v  0.5  0.5  0.5\n"
    "v -0.5  0.5  0.5\n"
    "v  0.5 -0.5 -0.5\n"
    "v -0.5 -0.5 -0.5\n"
    "v -0.5  0.5 -0.5\n"
    "v  0.5  0.5 -0.5\n"
    "f 1 2 3 4\n"
    "f 5 6 7 8\n"
    "f 2 5 8 3\n"
    "f 6 1 4 7\n"
    "f 4 3 8 7\n"
    "f 6 5 2 1\n";

static const char *k_ccw_red =
    "v -0.5 -0.5 0.5  0.91 0.31 0.31\n"
    "v  0.5 -0.5 0.5  0.91 0.31 0.31\n"
    "v  0.5  0.5 0.5  0.91 0.31 0.31\n"
    "v -0.5  0.5 0.5  0.91 0.31 0.31\n"
    "f 1 2 3 4\n";

static const char *k_cw_red =
    "v -0.5 -0.5 0.5  0.91 0.31 0.31\n"
    "v  0.5 -0.5 0.5  0.91 0.31 0.31\n"
    "v  0.5  0.5 0.5  0.91 0.31 0.31\n"
    "v -0.5  0.5 0.5  0.91 0.31 0.31\n"
    "f 1 4 3 2\n";

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

static void cpu_tests(void)
{
    struct wp_mesh_cpu g;
    struct wp_camera headon;
    float vp[16];
    float pa[3], pb[3], pc[3];
    int ret;
    uint32_t i;

    expect(wp_obj_load("/no/such/wayplot-obj-missing.obj", &g) == -ENOENT, "missing file is ENOENT");
    expect(g.v == NULL && g.nv == 0, "failed load does not leak a mesh");

    ret = wp_obj_parse("", 0, &g);
    expect(ret == -EINVAL, "empty text");
    ret = wp_obj_parse("v 0 0 0\nv 1 0 0\nv 0 1 0\n", strlen("v 0 0 0\nv 1 0 0\nv 0 1 0\n"), &g);
    expect(ret == -EINVAL, "positions without faces");
    {
        const char *s = "f 1 2 3\n";
        ret = wp_obj_parse(s, strlen(s), &g);
        expect(ret == -EINVAL, "face before any vertex");
    }
    {
        const char *s = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 99\n";
        ret = wp_obj_parse(s, strlen(s), &g);
        expect(ret == -EINVAL, "out-of-range index");
    }

    {
        const char *s = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
        ret = wp_obj_parse(s, strlen(s), &g);
    }
    expect(ret == 0 && g.nv == 3 && g.ni == 3, "one triangle");
    wp_mesh_cpu_free(&g);

    ret = wp_obj_parse(k_ccw_red, strlen(k_ccw_red), &g);
    expect(ret == 0 && g.nv == 6 && g.ni == 6, "quad fans to two triangles");
    wp_mesh_cpu_free(&g);

    {
        const char *rel = "v -0.5 -0.5 0.5\nv 0.5 -0.5 0.5\nv 0.5 0.5 0.5\nv -0.5 0.5 0.5\n"
                          "f -4 -3 -2 -1\n";
        ret = wp_obj_parse(rel, strlen(rel), &g);
        expect(ret == 0 && g.ni == 6, "relative negative indices");
        expect(g.v[0].px == -0.5f && g.v[1].px == 0.5f, "relative face uses last four verts");
        wp_mesh_cpu_free(&g);
    }

    {
        const char *vn = "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nf 1//1 2//1 3//1\n";
        ret = wp_obj_parse(vn, strlen(vn), &g);
        expect(ret == 0 && g.nv == 3, "v//vn");
        expect(g.v[0].nz > 0.9f && g.v[1].nz > 0.9f, "indexed vn is used (not a guessed face)");
        wp_mesh_cpu_free(&g);
    }

    ret = wp_obj_parse(k_cube, strlen(k_cube), &g);
    expect(ret == 0 && g.ni == 36 && g.nv == 36, "cube OBJ is 12 triangles (unique verts)");
    wp_camera_default(&headon);
    headon.eye[0] = 0.0f;
    headon.eye[1] = 0.0f;
    headon.eye[2] = 2.5f;
    wp_camera_vp(&headon, 16.0f / 9.0f, vp);
    pa[0] = g.v[g.idx[0]].px;
    pa[1] = g.v[g.idx[0]].py;
    pa[2] = g.v[g.idx[0]].pz;
    pb[0] = g.v[g.idx[1]].px;
    pb[1] = g.v[g.idx[1]].py;
    pb[2] = g.v[g.idx[1]].pz;
    pc[0] = g.v[g.idx[2]].px;
    pc[1] = g.v[g.idx[2]].py;
    pc[2] = g.v[g.idx[2]].pz;
    expect(wp_triangle_front_facing(vp, pa, pb, pc), "cube OBJ +Z first tri is a raster front");
    pa[0] = g.v[g.idx[6]].px;
    pa[1] = g.v[g.idx[6]].py;
    pa[2] = g.v[g.idx[6]].pz;
    pb[0] = g.v[g.idx[7]].px;
    pb[1] = g.v[g.idx[7]].py;
    pb[2] = g.v[g.idx[7]].pz;
    pc[0] = g.v[g.idx[8]].px;
    pc[1] = g.v[g.idx[8]].py;
    pc[2] = g.v[g.idx[8]].pz;
    expect(!wp_triangle_front_facing(vp, pa, pb, pc), "cube OBJ -Z first tri is a back face");
    wp_mesh_cpu_free(&g);

    ret = wp_obj_load("test/renderer/cube.obj", &g);
    expect(ret == 0 && g.ni == 36, "load cube.obj from path");
    wp_mesh_cpu_free(&g);

    {
        char tmpl[] = "/tmp/wayplot-obj-XXXXXX";
        int fd = mkstemp(tmpl);
        expect(fd >= 0, "tmpfile");
        if (fd >= 0) {
            ssize_t w = write(fd, k_cube, strlen(k_cube));
            close(fd);
            expect(w == (ssize_t)strlen(k_cube), "wrote tmp OBJ");
            ret = wp_obj_load(tmpl, &g);
            expect(ret == 0 && g.ni == 36, "wp_obj_load of a written file");
            wp_mesh_cpu_free(&g);
            unlink(tmpl);
        }
    }

    {
        const uint32_t nquad = 10000;
        size_t cap = (size_t)nquad * 96u + 64u, o = 0;
        char *buf = malloc(cap);
        uint64_t t0, t1;
        expect(buf != NULL, "grid buffer");
        if (buf) {
            for (i = 0; i < nquad; i++) {
                int n = snprintf(buf + o, cap - o, "v %u 0 0\nv %u 0 0\nv %u 1 0\nv %u 1 0\nf %u %u %u %u\n",
                                 i, i + 1, i + 1, i, 4 * i + 1, 4 * i + 2, 4 * i + 3, 4 * i + 4);
                if (n < 0 || (size_t)n >= cap - o) {
                    g_fail++;
                    break;
                }
                o += (size_t)n;
            }
            t0 = now_ns();
            ret = wp_obj_parse(buf, o, &g);
            t1 = now_ns();
            expect(ret == 0 && g.ni == nquad * 6, "parse 10k quads");
            printf("      parse %u quads  %u tris  %.2f ns/tri  %zu bytes\n", nquad, g.ni / 3,
                   (double)(t1 - t0) / (double)(g.ni / 3), o);
            wp_mesh_cpu_free(&g);
            free(buf);
        }
    }
}

int main(void)
{
    struct wp_session *s;
    struct wp_present *p;
    struct wp_pass pass;
    struct wp_lit lit;
    struct wp_mesh mesh;
    struct wp_mesh_cpu cpu;
    struct wp_camera cam;
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
    cam.eye[0] = 0.0f;
    cam.eye[1] = 0.0f;
    cam.eye[2] = 2.5f;

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

    ret = wp_obj_parse(k_ccw_red, strlen(k_ccw_red), &cpu);
    expect(ret == 0, "parse CCW red +Z quad");
    ret = wp_mesh_upload(&p->device, &mesh, cpu.v, cpu.nv, cpu.idx, cpu.ni);
    expect(ret == 0, "upload CCW OBJ");
    wp_mesh_cpu_free(&cpu);
    memset(pix, 0, (size_t)RW * RH * 4);
    ret = render_to(&p->device, &pass, &lit, &mesh, &cam, model, pix);
    expect(ret == 0, "GPU readback CCW OBJ");
    if (ret == 0) {
        c = px_at(pix, RW / 2, RH / 2);
        printf("      CCW center RGBA %u %u %u %u\n", c[0], c[1], c[2], c[3]);
        expect(is_plus_z(c), "CCW OBJ +Z is red at center (not culled, not a factory cube)");
        expect(!is_clear(c), "CCW OBJ is not the clear color");
    }
    wp_mesh_destroy(&p->device, &mesh);

    ret = wp_obj_parse(k_cw_red, strlen(k_cw_red), &cpu);
    expect(ret == 0, "parse clockwise red quad");
    ret = wp_mesh_upload(&p->device, &mesh, cpu.v, cpu.nv, cpu.idx, cpu.ni);
    expect(ret == 0, "upload clockwise OBJ");
    wp_mesh_cpu_free(&cpu);
    memset(pix, 0, (size_t)RW * RH * 4);
    ret = render_to(&p->device, &pass, &lit, &mesh, &cam, model, pix);
    expect(ret == 0, "GPU readback clockwise OBJ");
    if (ret == 0) {
        c = px_at(pix, RW / 2, RH / 2);
        printf("      CW  center RGBA %u %u %u %u\n", c[0], c[1], c[2], c[3]);
        expect(is_clear(c), "clockwise OBJ is culled (CULL_BACK, not CULL_NONE)");
        expect(!is_plus_z(c), "clockwise is not drawn as +Z");
    }
    wp_mesh_destroy(&p->device, &mesh);

    ret = wp_obj_load("test/renderer/cube.obj", &cpu);
    expect(ret == 0, "load fixture cube.obj");
    ret = wp_mesh_upload(&p->device, &mesh, cpu.v, cpu.nv, cpu.idx, cpu.ni);
    expect(ret == 0, "upload cube.obj");
    wp_mesh_cpu_free(&cpu);

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

    wp_camera_default(&cam);
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
        wp_mat4_identity(model);
        wp_pass_opaque_begin(&pass, f.cmd, f.view, f.extent.width, f.extent.height, f.slot);
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
    printf("      obj frames %u  %ux%u\n", presented, p->sc.width, p->sc.height);
    expect(presented >= 2, "presented frames of a file mesh");

done:
    wp_mesh_cpu_free(&cpu);
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

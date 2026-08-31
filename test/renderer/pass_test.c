#define _GNU_SOURCE
#include "engine/present.h"
#include "helper/math3d.h"
#include "renderer/camera.h"
#include "renderer/font.h"
#include "renderer/lit.h"
#include "renderer/mesh.h"
#include "renderer/pass.h"
#include "renderer/text.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* GPU lock: one command buffer, two render scopes (opaque cube then overlay
 * glyph). Draws must not BeginRendering. Heap session/present. */

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

static int is_plus_z(const uint8_t *p)
{
    return p[0] > 50 && p[0] > (unsigned)p[1] * 3 / 2 && p[0] > p[2];
}

static int render_both(struct wp_device *d, struct wp_pass *pass, struct wp_lit *lit,
                       struct wp_mesh *mesh, const struct wp_camera *cam, const float model[16],
                       struct wp_text *txt, struct wp_font *font, const struct wp_text_geom *geom,
                       const float rgba[4], uint8_t *pixels)
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
    wp_pass_overlay_begin(pass, cmd, view, RW, RH);
    wp_text_draw(txt, cmd, RW, RH, 0, font, geom, rgba);
    wp_pass_overlay_end(cmd);
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
    struct wp_mesh mesh;
    struct wp_lit lit;
    struct wp_font font;
    struct wp_text txt;
    struct wp_text_geom geom;
    struct wp_camera cam;
    struct wp_present_frame f;
    float model[16];
    float white[4] = { 1, 1, 1, 1 };
    uint8_t *pix;
    const uint8_t *c;
    unsigned covered = 0, far_hit = 0, presented = 0;
    uint64_t deadline, t0;
    int ret, x, y, x0, y0, x1, y1;

    s = calloc(1, sizeof(*s));
    p = calloc(1, sizeof(*p));
    memset(&pass, 0, sizeof(pass));
    memset(&mesh, 0, sizeof(mesh));
    memset(&lit, 0, sizeof(lit));
    memset(&font, 0, sizeof(font));
    memset(&txt, 0, sizeof(txt));
    memset(&geom, 0, sizeof(geom));
    pix = calloc((size_t)RW * RH, 4);
    if (!s || !p || !pix) {
        free(s);
        free(p);
        free(pix);
        return 1;
    }
    wp_mat4_identity(model);

    ret = wp_font_open_default(&font, 32.0f);
    expect(ret == 0, "font open");
    if (ret < 0)
        goto done;

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

    ret = wp_pass_init(&pass, &p->device, VK_FORMAT_R8G8B8A8_UNORM, 64, 64);
    expect(ret == 0 && pass.width == 64 && pass.height == 64, "pass init 64x64");
    ret = wp_pass_resize(&pass, RW, RH);
    expect(ret == 0 && pass.width == RW && pass.height == RH, "pass resize to 128x128");
    if (ret < 0)
        goto done;

    ret = wp_mesh_cube(&p->device, &mesh);
    expect(ret == 0, "mesh");
    ret = wp_lit_init(&lit, &p->device, VK_FORMAT_R8G8B8A8_UNORM);
    expect(ret == 0, "lit (no depth of its own)");
    if (ret < 0)
        goto done;
    ret = wp_font_upload(&font, &p->device);
    expect(ret == 0, "atlas host-copy");
    ret = wp_text_init(&txt, &p->device, VK_FORMAT_R8G8B8A8_UNORM);
    expect(ret == 0, "text (no BeginRendering)");
    if (ret < 0)
        goto done;
    ret = wp_text_layout(&font, "H", 8, 8, &geom);
    expect(ret == 0 && geom.ni == 6, "layout H");

    wp_camera_default(&cam);
    cam.eye[0] = 0.0f;
    cam.eye[1] = 0.0f;
    cam.eye[2] = 2.5f;
    ret = render_both(&p->device, &pass, &lit, &mesh, &cam, model, &txt, &font, &geom, white, pix);
    expect(ret == 0, "GPU readback of opaque cube + overlay H in one cmd");
    if (ret == 0) {
        c = px_at(pix, RW / 2, RH / 2);
        printf("      center RGBA %u %u %u %u\n", c[0], c[1], c[2], c[3]);
        expect(is_plus_z(c), "center is the +Z (red) outward face after overlay LOAD");
        x0 = (int)geom.x0;
        y0 = (int)geom.y0;
        x1 = (int)geom.x1;
        y1 = (int)geom.y1;
        if (x0 < 0)
            x0 = 0;
        if (y0 < 0)
            y0 = 0;
        if (x1 > RW)
            x1 = RW;
        if (y1 > RH)
            y1 = RH;
        for (y = y0; y < y1; y++) {
            for (x = x0; x < x1; x++) {
                uint8_t *px = px_at(pix, x, y);
                if (px[0] > 80 && px[1] > 80 && px[2] > 80)
                    covered++;
            }
        }
        for (y = 0; y < 12; y++) {
            for (x = RW - 24; x < RW; x++) {
                uint8_t *px = px_at(pix, x, y);
                if (px[0] > 80 && px[1] > 80 && px[2] > 80)
                    far_hit++;
            }
        }
        printf("      H AABB [%d,%d]x[%d,%d]  covered %u  far %u\n", x0, x1, y0, y1, covered, far_hit);
        expect(covered > 20, "glyph coverage inside the layout AABB (overlay after opaque)");
        expect(far_hit == 0, "no glyph ink in the opposite corner");
        expect((int)geom.x1 < RW / 2 && (int)geom.y1 < RH / 2,
               "H AABB does not cover the cube center (two scopes, not one smeared draw)");
    }

    wp_pass_destroy(&pass);
    wp_text_destroy(&txt);
    wp_lit_destroy(&lit);
    ret = wp_pass_init(&pass, &p->device, p->negotiated.vk_format, p->sc.width, p->sc.height);
    expect(ret == 0, "pass matching swapchain");
    if (ret < 0)
        goto done;
    ret = wp_lit_init(&lit, &p->device, p->negotiated.vk_format);
    expect(ret == 0, "lit matching swapchain format");
    if (ret < 0)
        goto done;
    ret = wp_text_init(&txt, &p->device, p->negotiated.vk_format);
    expect(ret == 0, "text matching swapchain format");
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
        wp_lit_draw(&lit, f.cmd, f.extent.width, f.extent.height, f.slot, &mesh, &cam, model);
        wp_pass_opaque_end(f.cmd);
        wp_pass_overlay_begin(&pass, f.cmd, f.view, f.extent.width, f.extent.height);
        wp_text_draw(&txt, f.cmd, f.extent.width, f.extent.height, f.slot, &font, &geom, white);
        wp_pass_overlay_end(f.cmd);
        ret = wp_present_end(p, &f);
        if (ret < 0) {
            g_fail++;
            break;
        }
        presented++;
    }
    printf("      folded frames %u  %ux%u\n", presented, p->sc.width, p->sc.height);
    expect(presented >= 2, "presented two-pass frames on the swapchain");

done:
    wp_text_geom_free(&geom);
    wp_text_destroy(&txt);
    wp_lit_destroy(&lit);
    wp_pass_destroy(&pass);
    if (p)
        wp_mesh_destroy(&p->device, &mesh);
    wp_font_destroy(&font);
    wp_present_close(p);
    wp_session_close(s);
    free(p);
    free(s);
    free(pix);
    printf("%s  %d failure(s)\n", g_fail ? "RESULT FAIL" : "RESULT PASS", g_fail);
    return g_fail ? 1 : 0;
}

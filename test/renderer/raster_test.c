#define _GNU_SOURCE
#include "engine/present.h"
#include "helper/math3d.h"
#include "renderer/camera.h"
#include "renderer/lit.h"
#include "renderer/mesh.h"
#include "renderer/pass.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* GPU pixel lock: a face is in the framebuffer iff the camera sees its
 * outward side. CPU winding tests cannot see a wrong VkFrontFace mapping
 * (that was the inside-out cube). This reads the rasterizer. */

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

static uint8_t *px_at(uint8_t *p, int x, int y)
{
    return p + ((size_t)y * RW + (size_t)x) * 4;
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
    VkHostImageLayoutTransitionInfo tr = {
        .sType = VK_STRUCTURE_TYPE_HOST_IMAGE_LAYOUT_TRANSITION_INFO,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
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

    tr.image = img;
    (void)vkTransitionImageLayout(d->device, 1, &tr);
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

static int is_clear(const uint8_t *p)
{
    return p[0] < 40 && p[1] > 20 && p[1] < 70 && p[2] > 30 && p[2] < 90 && p[0] + 8 < p[2];
}

/* +Z vertex (0.91,0.31,0.31) stays R-dominant after any uniform-ish light. */
static int is_plus_z(const uint8_t *p)
{
    return p[0] > 50 && p[0] > (unsigned)p[1] * 3 / 2 && p[0] > p[2];
}

/* -Z vertex (0.31,0.72,0.38): green, not cyan. +Y is (g≈b) cyan. */
static int is_minus_z(const uint8_t *p)
{
    return p[1] > 30 && p[1] > (unsigned)p[0] * 3 / 2 && (int)p[1] > (int)p[2] + 20;
}

/* +Y vertex (0.28,0.84,0.84). */
static int is_plus_y(const uint8_t *p)
{
    int d = (int)p[1] - (int)p[2];
    if (d < 0)
        d = -d;
    return p[1] > 80 && p[2] > 80 && d < 40;
}

static void count_faces(uint8_t *pix, unsigned *n_clear, unsigned *n_pz, unsigned *n_mz,
                       unsigned *n_py)
{
    int x, y;
    *n_clear = *n_pz = *n_mz = *n_py = 0;
    for (y = 0; y < RH; y++) {
        for (x = 0; x < RW; x++) {
            uint8_t *p = px_at(pix, x, y);
            if (is_clear(p))
                (*n_clear)++;
            else if (is_plus_z(p))
                (*n_pz)++;
            else if (is_plus_y(p))
                (*n_py)++;
            else if (is_minus_z(p))
                (*n_mz)++;
        }
    }
}

int main(void)
{
    struct wp_session *s;
    struct wp_present *p;
    struct wp_pass pass;
    struct wp_mesh mesh;
    struct wp_lit lit;
    struct wp_camera cam;
    float model[16];
    uint8_t *pix;
    unsigned n_clear, n_pz, n_mz, n_py;
    const uint8_t *c;
    int ret;

    s = calloc(1, sizeof(*s));
    p = calloc(1, sizeof(*p));
    memset(&pass, 0, sizeof(pass));
    memset(&mesh, 0, sizeof(mesh));
    memset(&lit, 0, sizeof(lit));
    pix = calloc((size_t)RW * RH, 4);
    if (!s || !p || !pix) {
        free(s);
        free(p);
        free(pix);
        return 1;
    }
    wp_mat4_identity(model);

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
    ret = wp_mesh_cube(&p->device, &mesh);
    expect(ret == 0, "mesh");
    ret = wp_pass_init(&pass, &p->device, VK_FORMAT_R8G8B8A8_UNORM, RW, RH);
    expect(ret == 0, "pass");
    if (ret < 0)
        goto done;
    ret = wp_lit_init(&lit, &p->device, VK_FORMAT_R8G8B8A8_UNORM);
    expect(ret == 0, "lit");
    if (ret < 0)
        goto done;

    /* Head-on +Z: only the red face may fill the center. Green = insides. */
    wp_camera_default(&cam);
    cam.eye[0] = 0.0f;
    cam.eye[1] = 0.0f;
    cam.eye[2] = 2.5f;
    ret = render_to(&p->device, &pass, &lit, &mesh, &cam, model, pix);
    expect(ret == 0, "head-on GPU readback");
    if (ret == 0) {
        c = px_at(pix, RW / 2, RH / 2);
        printf("      head-on center RGBA %u %u %u %u\n", c[0], c[1], c[2], c[3]);
        expect(!is_clear(c), "head-on: cube covers the image center (not an empty window)");
        expect(is_plus_z(c), "head-on: center is the +Z (red) outward face");
        expect(!is_minus_z(c), "head-on: center is NOT the -Z (green) inner face");
        count_faces(pix, &n_clear, &n_pz, &n_mz, &n_py);
        printf("      head-on pixels  clear %u  +Z %u  +Y %u  -Z %u\n", n_clear, n_pz, n_py, n_mz);
        expect(n_pz > 100, "head-on: a real +Z quad was rasterized");
        expect(n_mz < n_pz / 8, "head-on: -Z (back) is culled, not drawn");
        expect(n_py == 0, "head-on: +Y is edge-on / back, not drawn");
    }

    /* Corner view: the three outward faces, not the three far inner faces. */
    wp_camera_default(&cam);
    ret = render_to(&p->device, &pass, &lit, &mesh, &cam, model, pix);
    expect(ret == 0, "3/4 GPU readback");
    if (ret == 0) {
        count_faces(pix, &n_clear, &n_pz, &n_mz, &n_py);
        printf("      3/4 pixels  clear %u  +Z %u  +Y %u  -Z %u\n", n_clear, n_pz, n_py, n_mz);
        expect(n_pz > 50, "3/4: +Z outward face is visible");
        expect(n_py > 50, "3/4: +Y outward face is visible");
        expect(n_mz < 20, "3/4: -Z inner face is culled (inside-out cube would show green instead of red)");
    }

done:
    wp_lit_destroy(&lit);
    wp_pass_destroy(&pass);
    if (p)
        wp_mesh_destroy(&p->device, &mesh);
    wp_present_close(p);
    wp_session_close(s);
    free(p);
    free(s);
    free(pix);
    printf("%s  %d failure(s)\n", g_fail ? "RESULT FAIL" : "RESULT PASS", g_fail);
    return g_fail ? 1 : 0;
}

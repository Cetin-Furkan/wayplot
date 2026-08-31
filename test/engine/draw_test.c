#define _GNU_SOURCE
#include "engine/draw.h"
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

/* CPU list + GPU lock: two overlay items, change a rect, pixels follow.
 * Heap session/present. See docs/LIST.md. */

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

static int is_white(const uint8_t *p)
{
    return p[0] > 80 && p[1] > 80 && p[2] > 80;
}

/* Yellow overlay: R and G high, B well below R. White fails this. */
static int is_yellow(const uint8_t *p)
{
    return p[0] > 80 && p[1] > 80 && (int)p[0] > (int)p[2] + 40 && (int)p[1] > (int)p[2] + 40;
}

static void clamp_aabb(const struct wp_text_geom *g, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = (int)g->x0;
    *y0 = (int)g->y0;
    *x1 = (int)g->x1;
    *y1 = (int)g->y1;
    if (*x0 < 0)
        *x0 = 0;
    if (*y0 < 0)
        *y0 = 0;
    if (*x1 > RW)
        *x1 = RW;
    if (*y1 > RH)
        *y1 = RH;
}

static unsigned count_pred(uint8_t *pix, int x0, int y0, int x1, int y1, int (*pred)(const uint8_t *))
{
    unsigned n = 0;
    int x, y;
    for (y = y0; y < y1; y++) {
        for (x = x0; x < x1; x++) {
            if (pred(px_at(pix, x, y)))
                n++;
        }
    }
    return n;
}

static int render_list(struct wp_device *d, struct wp_pass *pass, struct wp_draw_list *list,
                       struct wp_lit *lit, struct wp_text *txt, uint8_t *pixels)
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
    wp_draw_list_record(list, pass, lit, txt, NULL, cmd, view, RW, RH, 0);
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
    struct wp_draw_list list;
    struct wp_mesh mesh;
    struct wp_camera cam;
    struct wp_font font;
    struct wp_text_geom geom;
    float model[16];
    float rgba[4] = { 0.2f, 0.4f, 0.6f, 1.0f };
    float ident0;
    uint32_t i;
    int ret;

    memset(&mesh, 0, sizeof(mesh));
    memset(&font, 0, sizeof(font));
    memset(&geom, 0, sizeof(geom));
    wp_camera_default(&cam);
    wp_mat4_identity(model);
    ident0 = model[0];

    wp_draw_list_init(&list);
    expect(wp_draw_list_count(&list) == 0, "init count 0");
    expect(wp_draw_list_push_lit(NULL, &mesh, &cam, model) == -EINVAL, "push_lit null list");
    expect(wp_draw_list_push_lit(&list, NULL, &cam, model) == -EINVAL, "push_lit null mesh");
    expect(wp_draw_list_push_lit(&list, &mesh, NULL, model) == -EINVAL, "push_lit null camera");
    expect(wp_draw_list_push_lit(&list, &mesh, &cam, NULL) == -EINVAL, "push_lit null model");
    expect(wp_draw_list_push_text(NULL, &font, &geom, rgba) == -EINVAL, "push_text null list");
    expect(wp_draw_list_push_text(&list, NULL, &geom, rgba) == -EINVAL, "push_text null font");
    expect(wp_draw_list_push_text(&list, &font, NULL, rgba) == -EINVAL, "push_text null geom");
    expect(wp_draw_list_push_text(&list, &font, &geom, NULL) == -EINVAL, "push_text null rgba");

    ret = wp_draw_list_push_lit(&list, &mesh, &cam, model);
    expect(ret == 0 && wp_draw_list_count(&list) == 1, "push one lit");
    model[0] = 99.0f;
    expect(list.items[0].lit.model[0] == ident0,
           "list copied the model (caller stack can change after push)");
    expect(list.items[0].kind == WP_DRAW_LIT, "kind is lit");

    ret = wp_draw_list_push_text(&list, &font, &geom, rgba);
    expect(ret == 0 && wp_draw_list_count(&list) == 2, "push one text");
    rgba[0] = 0.0f;
    expect(list.items[1].text.rgba[0] > 0.1f,
           "list copied rgba (caller stack can change after push)");
    expect(wp_draw_list_count_kind(&list, WP_DRAW_LIT) == 1, "one lit item");
    expect(wp_draw_list_count_kind(&list, WP_DRAW_TEXT) == 1, "one text item");

    wp_mat4_identity(model);
    rgba[0] = 0.2f;
    for (i = 0; i < 38; i++) {
        if ((i % 2u) == 0)
            ret = wp_draw_list_push_lit(&list, &mesh, &cam, model);
        else
            ret = wp_draw_list_push_text(&list, &font, &geom, rgba);
        if (ret < 0)
            break;
    }
    expect(ret == 0 && wp_draw_list_count(&list) == 40, "grew to 40 items");
    expect(list.cap >= 40, "cap grew (not a fixed 8-slot toy)");
    expect(wp_draw_list_count_kind(&list, WP_DRAW_LIT) == 20, "20 lit after grow");
    expect(wp_draw_list_count_kind(&list, WP_DRAW_TEXT) == 20, "20 text after grow");

    {
        uint32_t cap = list.cap;
        wp_draw_list_clear(&list);
        expect(wp_draw_list_count(&list) == 0, "clear zeros count");
        expect(list.cap == cap && list.items != NULL, "clear keeps the allocation");
    }

    for (i = 0; i < WP_DRAW_MAX; i++) {
        ret = wp_draw_list_push_lit(&list, &mesh, &cam, model);
        if (ret < 0)
            break;
    }
    expect(ret == 0 && wp_draw_list_count(&list) == WP_DRAW_MAX, "filled WP_DRAW_MAX");
    expect(wp_draw_list_push_lit(&list, &mesh, &cam, model) == -ENOSPC, "one past max is -ENOSPC");
    expect(wp_draw_list_count(&list) == WP_DRAW_MAX, "count stays at max after ENOSPC");

    wp_draw_list_destroy(&list);
    expect(list.items == NULL && list.count == 0 && list.cap == 0, "destroy frees");
}

int main(void)
{
    struct wp_session *s;
    struct wp_present *p;
    struct wp_pass pass;
    struct wp_draw_list list;
    struct wp_mesh mesh;
    struct wp_lit lit;
    struct wp_font font;
    struct wp_text txt;
    struct wp_text_geom geom_a, geom_b, geom_old;
    struct wp_camera cam;
    struct wp_present_frame f;
    float model[16];
    float white[4] = { 1, 1, 1, 1 };
    float yellow[4] = { 1.0f, 0.9f, 0.15f, 1.0f };
    uint8_t *pix;
    const uint8_t *c;
    unsigned n_white, n_yellow, presented = 0;
    uint64_t deadline, t0;
    int ret, x0, y0, x1, y1, ox0, oy0, ox1, oy1;

    cpu_tests();

    s = calloc(1, sizeof(*s));
    p = calloc(1, sizeof(*p));
    memset(&pass, 0, sizeof(pass));
    wp_draw_list_init(&list);
    memset(&mesh, 0, sizeof(mesh));
    memset(&lit, 0, sizeof(lit));
    memset(&font, 0, sizeof(font));
    memset(&txt, 0, sizeof(txt));
    memset(&geom_a, 0, sizeof(geom_a));
    memset(&geom_b, 0, sizeof(geom_b));
    memset(&geom_old, 0, sizeof(geom_old));
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

    ret = wp_pass_init(&pass, &p->device, VK_FORMAT_R8G8B8A8_UNORM, RW, RH);
    expect(ret == 0, "pass");
    if (ret < 0)
        goto done;
    ret = wp_mesh_cube(&p->device, &mesh);
    expect(ret == 0, "mesh");
    ret = wp_lit_init(&lit, &p->device, VK_FORMAT_R8G8B8A8_UNORM);
    expect(ret == 0, "lit");
    if (ret < 0)
        goto done;
    ret = wp_font_upload(&font, &p->device);
    expect(ret == 0, "atlas host-copy");
    ret = wp_text_init(&txt, &p->device, VK_FORMAT_R8G8B8A8_UNORM);
    expect(ret == 0, "text");
    if (ret < 0)
        goto done;

    wp_camera_default(&cam);
    cam.eye[0] = 0.0f;
    cam.eye[1] = 0.0f;
    cam.eye[2] = 2.5f;

    ret = render_list(&p->device, &pass, &list, &lit, &txt, pix);
    expect(ret == 0, "GPU readback of an empty list");
    if (ret == 0) {
        c = px_at(pix, RW / 2, RH / 2);
        printf("      empty center RGBA %u %u %u %u\n", c[0], c[1], c[2], c[3]);
        expect(is_clear(c), "empty list still clears (opaque pass ran, not a skipped record)");
    }

    ret = wp_text_layout(&font, "H", 8, 8, &geom_a);
    expect(ret == 0 && geom_a.ni == 6, "layout white H at (8,8)");
    ret = wp_text_layout(&font, "H", 96, 8, &geom_b);
    expect(ret == 0 && geom_b.ni == 6, "layout yellow H at (96,8)");
    expect((int)geom_a.x1 < RW / 2 && (int)geom_b.x0 > RW / 2,
           "the two H AABBs sit on opposite sides of the cube center");

    ret = wp_draw_list_push_lit(&list, &mesh, &cam, model);
    expect(ret == 0, "push cube");
    ret = wp_draw_list_push_text(&list, &font, &geom_a, white);
    expect(ret == 0, "push white H");
    ret = wp_draw_list_push_text(&list, &font, &geom_b, yellow);
    expect(ret == 0, "push yellow H");
    expect(wp_draw_list_count(&list) == 3, "three items (one opaque, two overlay)");
    expect(wp_draw_list_count_kind(&list, WP_DRAW_TEXT) == 2, "two overlay items");

    memset(pix, 0, (size_t)RW * RH * 4);
    ret = render_list(&p->device, &pass, &list, &lit, &txt, pix);
    expect(ret == 0, "GPU readback of cube + two H via the list");
    if (ret == 0) {
        c = px_at(pix, RW / 2, RH / 2);
        printf("      cube center RGBA %u %u %u %u\n", c[0], c[1], c[2], c[3]);
        expect(is_plus_z(c), "center is the +Z (red) outward face");
        clamp_aabb(&geom_a, &x0, &y0, &x1, &y1);
        n_white = count_pred(pix, x0, y0, x1, y1, is_white);
        n_yellow = count_pred(pix, x0, y0, x1, y1, is_yellow);
        printf("      white H AABB [%d,%d]x[%d,%d]  white %u  yellow %u\n",
               x0, x1, y0, y1, n_white, n_yellow);
        expect(n_white > 20, "white H coverage in its AABB");
        expect(n_yellow == 0, "white H AABB is not the yellow UBO (second item did not overwrite first)");
        clamp_aabb(&geom_b, &x0, &y0, &x1, &y1);
        n_white = count_pred(pix, x0, y0, x1, y1, is_white);
        n_yellow = count_pred(pix, x0, y0, x1, y1, is_yellow);
        printf("      yellow H AABB [%d,%d]x[%d,%d]  white %u  yellow %u\n",
               x0, x1, y0, y1, n_white, n_yellow);
        expect(n_yellow > 20, "yellow H coverage in its AABB");
        expect(n_white == 0, "yellow H AABB is not white (first UBO did not paint both)");
    }

    /* Change a rect on the CPU. Same list item still points at geom_b. */
    geom_old = geom_b;
    geom_old.v = NULL;
    geom_old.idx = NULL;
    wp_text_geom_free(&geom_b);
    ret = wp_text_layout(&font, "H", 96, 90, &geom_b);
    expect(ret == 0 && geom_b.ni == 6, "relayout yellow H at (96,90) (CPU rect change)");
    expect(list.items[2].text.geom == &geom_b, "list still points at the same geom object");

    memset(pix, 0, (size_t)RW * RH * 4);
    ret = render_list(&p->device, &pass, &list, &lit, &txt, pix);
    expect(ret == 0, "GPU readback after CPU moved the yellow rect");
    if (ret == 0) {
        c = px_at(pix, RW / 2, RH / 2);
        expect(is_plus_z(c), "cube center still +Z red after the overlay moved");
        clamp_aabb(&geom_old, &ox0, &oy0, &ox1, &oy1);
        n_yellow = count_pred(pix, ox0, oy0, ox1, oy1, is_yellow);
        printf("      old yellow AABB [%d,%d]x[%d,%d]  yellow %u\n", ox0, ox1, oy0, oy1, n_yellow);
        expect(n_yellow == 0, "old yellow rect is empty (GPU followed the CPU move)");
        clamp_aabb(&geom_b, &x0, &y0, &x1, &y1);
        n_yellow = count_pred(pix, x0, y0, x1, y1, is_yellow);
        printf("      new yellow AABB [%d,%d]x[%d,%d]  yellow %u\n", x0, x1, y0, y1, n_yellow);
        expect(n_yellow > 20, "new yellow rect has ink");
        clamp_aabb(&geom_a, &x0, &y0, &x1, &y1);
        n_white = count_pred(pix, x0, y0, x1, y1, is_white);
        expect(n_white > 20, "white H did not move");
    }

    wp_draw_list_clear(&list);
    ret = wp_draw_list_push_lit(&list, &mesh, &cam, model);
    expect(ret == 0 && wp_draw_list_count(&list) == 1, "cleared list, cube only");
    memset(pix, 0, (size_t)RW * RH * 4);
    ret = render_list(&p->device, &pass, &list, &lit, &txt, pix);
    expect(ret == 0, "GPU readback after dropping overlay items");
    if (ret == 0) {
        c = px_at(pix, RW / 2, RH / 2);
        expect(is_plus_z(c), "cube remains after overlay items were dropped");
        clamp_aabb(&geom_a, &x0, &y0, &x1, &y1);
        n_white = count_pred(pix, x0, y0, x1, y1, is_white);
        expect(n_white == 0, "white H gone when not in the list");
        clamp_aabb(&geom_b, &x0, &y0, &x1, &y1);
        n_yellow = count_pred(pix, x0, y0, x1, y1, is_yellow);
        expect(n_yellow == 0, "yellow H gone when not in the list");
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

    wp_draw_list_clear(&list);
    ret = wp_draw_list_push_lit(&list, &mesh, &cam, model);
    expect(ret == 0, "swapchain list: cube");
    ret = wp_draw_list_push_text(&list, &font, &geom_a, white);
    expect(ret == 0, "swapchain list: white H");
    ret = wp_draw_list_push_text(&list, &font, &geom_b, yellow);
    expect(ret == 0, "swapchain list: yellow H");

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
        wp_draw_list_record(&list, &pass, &lit, &txt, NULL, f.cmd, f.view, f.extent.width,
                            f.extent.height, f.slot);
        ret = wp_present_end(p, &f);
        if (ret < 0) {
            g_fail++;
            break;
        }
        presented++;
    }
    printf("      list frames %u  %ux%u\n", presented, p->sc.width, p->sc.height);
    expect(presented >= 2, "presented frames recorded from the list");

done:
    wp_text_geom_free(&geom_a);
    wp_text_geom_free(&geom_b);
    wp_draw_list_destroy(&list);
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

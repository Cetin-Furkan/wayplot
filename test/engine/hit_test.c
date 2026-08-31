#define _GNU_SOURCE
#include "engine/draw.h"
#include "engine/hit.h"
#include "engine/input.h"
#include "engine/present.h"
#include "helper/math3d.h"
#include "renderer/camera.h"
#include "renderer/card.h"
#include "renderer/lit.h"
#include "renderer/mesh.h"
#include "renderer/pass.h"
#include "wayland/session.h"

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

static int is_plus_z(const uint8_t *p)
{
    return p[0] > 50 && p[0] > (unsigned)p[1] * 3 / 2 && p[0] > p[2];
}

static int is_card(const uint8_t *p)
{
    return p[0] > 150 && p[2] > 100 && (int)p[0] > (int)p[1] + 80;
}

static int is_green(const uint8_t *p)
{
    return p[1] > 150 && (int)p[1] > (int)p[0] + 80 && (int)p[1] > (int)p[2] + 80;
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

static void cpu_tests(void)
{
    struct wp_hit_stack h;
    struct wp_hit_item it;
    struct wp_input in;
    struct wp_pointer p;
    struct wp_rect ra = { 8, 8, 32, 24 };
    struct wp_rect rb = { 80, 8, 32, 24 };
    struct wp_rect band = { 40, 16, 40, 20 };
    struct wp_rect corner = { 0, 0, 40, 40 };
    struct wp_rect bad = { 8, 8, 0, 24 };
    uint32_t i;
    int ret;

    wp_hit_clear(&h);
    expect(wp_hit_push(NULL, 1, ra) == -EINVAL, "push null stack");
    expect(wp_hit_push(&h, 0, ra) == -EINVAL, "id 0 rejected");
    expect(wp_hit_push(&h, 1, bad) == -EINVAL, "empty rect rejected");
    ret = wp_hit_push(&h, 1, ra);
    expect(ret == 0 && h.count == 1, "push A");
    ra.x = 99;
    expect(h.items[0].rect.x == 8, "stack copied the rect");
    ret = wp_hit_push(&h, 2, rb);
    expect(ret == 0 && h.count == 2, "push B");
    expect(wp_hit_pick(&h, 12, 12, &it) == 1 && it.id == 1, "pick A interior");
    expect(wp_hit_pick(&h, 90, 16, &it) == 1 && it.id == 2, "pick B interior");
    expect(wp_hit_pick(&h, 50, 16, NULL) == 0, "gap is a miss");
    expect(wp_hit_pick(&h, 8, 8, &it) == 1 && it.id == 1, "min edge is inside");
    expect(wp_hit_pick(&h, 40, 8, NULL) == 0, "max x is exclusive");
    expect(wp_hit_pick(&h, 8, 32, NULL) == 0, "max y is exclusive");

    ra = (struct wp_rect){ 8, 8, 32, 24 };
    {
        struct wp_rect over = { 16, 8, 32, 24 };
        wp_hit_clear(&h);
        expect(wp_hit_push(&h, 1, ra) == 0, "overlap bottom");
        expect(wp_hit_push(&h, 3, over) == 0, "overlap top");
        expect(wp_hit_pick(&h, 20, 12, &it) == 1 && it.id == 3, "last push wins overlap");
        expect(wp_hit_pick(&h, 10, 12, &it) == 1 && it.id == 1, "uncovered A still A");
    }

    wp_hit_clear(&h);
    for (i = 0; i < WP_HIT_MAX; i++) {
        struct wp_rect r = { (float)(i % 8) * 16.f, (float)(i / 8) * 8.f, 16, 8 };
        if (wp_hit_push(&h, i + 1, r) != 0) {
            g_fail++;
            printf("FAIL  fill stack at %u\n", i);
            break;
        }
    }
    expect(h.count == WP_HIT_MAX, "stack fills to cap");
    expect(wp_hit_push(&h, 99, ra) == -ENOSPC, "cap is ENOSPC");
    {
        uint32_t n = 200000, k, hits = 0;
        uint64_t t0, t1;
        t0 = now_ns();
        for (k = 0; k < n; k++)
            hits += (uint32_t)wp_hit_pick(&h, 8.f, 4.f, NULL);
        t1 = now_ns();
        printf("      pick %u items × %u  %.2f ns/pick  hits %u\n", WP_HIT_MAX, n,
               (double)(t1 - t0) / (double)n, hits);
        expect(hits == n, "top-left cell of the 64-grid is a hit");
    }

    wp_hit_clear(&h);
    expect(wp_hit_push(&h, 1, ra) == 0 && wp_hit_push(&h, 2, rb) == 0, "A+B for feed");
    wp_input_init(&in);
    memset(&p, 0, sizeof(p));
    p.inside = 1;
    p.x = 96;
    p.y = 20;
    p.pressed = WP_INPUT_LEFT;
    p.buttons = WP_INPUT_LEFT;
    wp_input_feed(&in, &p, &h, NULL, 0, 1280, 720, 0);
    expect(in.drag_id == 2 && in.chrome == WP_CHROME_NONE, "press on B starts drag, not chrome");
    expect(in.drag_rect.x == 80 && in.drag_rect.w == 32, "grab keeps B's origin and size");
    p.pressed = 0;
    p.x = 112;
    wp_input_feed(&in, &p, &h, NULL, 0, 1280, 720, 0);
    expect(in.drag_id == 2, "drag is sticky");
    expect(in.drag_rect.x == 96 && in.drag_rect.y == 8, "B follows pointer (dx=16)");
    expect(in.drag_rect.w == 32 && in.drag_rect.h == 24, "drag does not change w/h");
    p.x = 20;
    p.y = 20;
    wp_input_feed(&in, &p, &h, NULL, 0, 1280, 720, 0);
    expect(in.drag_id == 2 && in.drag_rect.x == 4, "motion over A does not re-pick");
    p.released = WP_INPUT_LEFT;
    p.buttons = 0;
    wp_input_feed(&in, &p, &h, NULL, 0, 1280, 720, 0);
    expect(in.drag_id == 0, "release ends drag");
    expect(in.drag_rect.x == 4 && in.drag_rect.y == 8, "release still writes the last sample");

    wp_input_init(&in);
    memset(&p, 0, sizeof(p));
    p.inside = 1;
    p.x = 640;
    p.y = 20;
    p.pressed = WP_INPUT_LEFT;
    wp_hit_clear(&h);
    wp_input_feed(&in, &p, &h, NULL, 0, 1280, 720, 0);
    expect(in.chrome == WP_CHROME_MOVE && in.drag_id == 0, "empty band is window move");
    p.x = 0;
    p.y = 0;
    in.chrome = WP_CHROME_NONE;
    wp_input_feed(&in, &p, &h, NULL, 0, 1280, 720, 0);
    expect(in.chrome == WP_CHROME_RESIZE && in.resize_edges == WP_RESIZE_TOP_LEFT,
           "corner is resize, not move");

    wp_hit_clear(&h);
    expect(wp_hit_push(&h, 4, band) == 0, "card sitting in the move band");
    wp_input_init(&in);
    memset(&p, 0, sizeof(p));
    p.inside = 1;
    p.x = 50;
    p.y = 24;
    p.pressed = WP_INPUT_LEFT;
    wp_input_feed(&in, &p, &h, NULL, 0, 1280, 720, 0);
    expect(in.drag_id == 4 && in.chrome == WP_CHROME_NONE,
           "card in the 36 px band drags (session must not eat the click)");

    wp_hit_clear(&h);
    expect(wp_hit_push(&h, 5, corner) == 0, "card covering the window corner");
    wp_input_init(&in);
    memset(&p, 0, sizeof(p));
    p.inside = 1;
    p.x = 2;
    p.y = 2;
    p.pressed = WP_INPUT_LEFT;
    wp_input_feed(&in, &p, &h, NULL, 0, 1280, 720, 0);
    expect(in.chrome == WP_CHROME_RESIZE && in.drag_id == 0, "12 px edge wins over a card");

    wp_input_init(&in);
    memset(&p, 0, sizeof(p));
    p.inside = 1;
    p.x = 640;
    p.y = 20;
    p.pressed = WP_INPUT_LEFT;
    wp_input_feed(&in, &p, NULL, NULL, 0, 1280, 720, WP_TOPLEVEL_FULLSCREEN);
    expect(in.chrome == WP_CHROME_NONE, "fullscreen suppresses move");
    p.x = 0;
    p.y = 0;
    wp_input_feed(&in, &p, NULL, NULL, 0, 1280, 720, WP_TOPLEVEL_MAXIMIZED);
    expect(in.chrome == WP_CHROME_MOVE, "maximized: corner is not resize, band still moves");
}

static int render_list(struct wp_device *d, struct wp_pass *pass, struct wp_draw_list *list,
                       struct wp_lit *lit, struct wp_card *card, uint8_t *pixels)
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
    wp_draw_list_record(list, pass, lit, NULL, card, cmd, view, RW, RH, 0);
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
    struct wp_draw_list list;
    struct wp_mesh mesh;
    struct wp_lit lit;
    struct wp_card card;
    struct wp_camera cam;
    struct wp_present_frame f;
    struct wp_rect ra, rb;
    struct wp_hit_stack hits;
    struct wp_input in;
    struct wp_pointer ptr;
    float model[16];
    float fill[4] = { 1.0f, 0.2f, 0.7f, 1.0f };
    float green[4] = { 0.2f, 0.9f, 0.3f, 1.0f };
    uint8_t *pix;
    const uint8_t *c;
    unsigned n_a, n_b, n_wrong, presented = 0;
    uint64_t deadline, t0;
    int ret;

    cpu_tests();

    s = calloc(1, sizeof(*s));
    p = calloc(1, sizeof(*p));
    memset(&pass, 0, sizeof(pass));
    wp_draw_list_init(&list);
    memset(&mesh, 0, sizeof(mesh));
    memset(&lit, 0, sizeof(lit));
    memset(&card, 0, sizeof(card));
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
    ret = wp_card_init(&card, &p->device, VK_FORMAT_R8G8B8A8_UNORM);
    expect(ret == 0, "card pipeline");
    if (ret < 0)
        goto done;

    wp_camera_default(&cam);
    cam.eye[0] = 0.0f;
    cam.eye[1] = 0.0f;
    cam.eye[2] = 2.5f;

    ra = (struct wp_rect){ 8, 8, 32, 24 };
    rb = (struct wp_rect){ 80, 8, 32, 24 };
    wp_hit_clear(&hits);
    wp_input_init(&in);
    memset(&ptr, 0, sizeof(ptr));
    expect(wp_hit_push(&hits, 1, ra) == 0 && wp_hit_push(&hits, 2, rb) == 0, "GPU hit stack");
    ptr.inside = 1;
    ptr.x = 96;
    ptr.y = 20;
    ptr.pressed = WP_INPUT_LEFT;
    ptr.buttons = WP_INPUT_LEFT;
    wp_input_feed(&in, &ptr, &hits, NULL, 0, RW, RH, 0);
    expect(in.drag_id == 2, "GPU path: press B");
    ptr.pressed = 0;
    ptr.x = 112;
    wp_input_feed(&in, &ptr, &hits, NULL, 0, RW, RH, 0);
    expect(in.drag_id == 2 && in.drag_rect.x == 96, "GPU path: B x += 16");
    rb.x = in.drag_rect.x;
    rb.y = in.drag_rect.y;

    ret = wp_draw_list_push_lit(&list, &mesh, &cam, model);
    expect(ret == 0, "push cube");
    ret = wp_draw_list_push_card(&list, &ra, fill);
    expect(ret == 0, "push A unmoved");
    ret = wp_draw_list_push_card(&list, &rb, green);
    expect(ret == 0, "push B after drag");

    memset(pix, 0, (size_t)RW * RH * 4);
    ret = render_list(&p->device, &pass, &list, &lit, &card, pix);
    expect(ret == 0, "GPU readback after dragging B");
    if (ret == 0) {
        c = px_at(pix, RW / 2, RH / 2);
        expect(is_plus_z(c), "dragged card is not a fullscreen panel");
        n_a = count_pred(pix, 8, 8, 40, 32, is_card);
        n_b = count_pred(pix, 96, 8, 128, 32, is_green);
        n_wrong = count_pred(pix, 80, 8, 96, 32, is_green);
        n_wrong += count_pred(pix, 8, 8, 40, 32, is_green);
        printf("      after drag  A %u  B %u  abandoned %u\n", n_a, n_b, n_wrong);
        expect(n_a > 700, "card A unmoved");
        expect(n_b > 700, "card B fills the new AABB");
        expect(n_wrong == 0, "pixels B left are empty of B (GPU followed the hit drag)");
    }

    wp_pass_destroy(&pass);
    wp_card_destroy(&card);
    wp_lit_destroy(&lit);
    ret = wp_pass_init(&pass, &p->device, p->negotiated.vk_format, p->sc.width, p->sc.height);
    expect(ret == 0, "pass matching swapchain");
    if (ret < 0)
        goto done;
    ret = wp_lit_init(&lit, &p->device, p->negotiated.vk_format);
    expect(ret == 0, "lit matching swapchain");
    if (ret < 0)
        goto done;
    ret = wp_card_init(&card, &p->device, p->negotiated.vk_format);
    expect(ret == 0, "card matching swapchain");
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
        wp_draw_list_record(&list, &pass, &lit, NULL, &card, f.cmd, f.view, f.extent.width,
                            f.extent.height, f.slot);
        ret = wp_present_end(p, &f);
        if (ret < 0) {
            g_fail++;
            break;
        }
        presented++;
    }
    printf("      hit frames %u  %ux%u\n", presented, p->sc.width, p->sc.height);
    expect(presented >= 2, "presented frames with dragged cards on the list");

done:
    wp_draw_list_destroy(&list);
    wp_card_destroy(&card);
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

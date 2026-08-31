#define _GNU_SOURCE
#include "engine/draw.h"
#include "engine/present.h"
#include "helper/math3d.h"
#include "renderer/camera.h"
#include "renderer/card.h"
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

static int is_plus_z(const uint8_t *p)
{
    return p[0] > 50 && p[0] > (unsigned)p[1] * 3 / 2 && p[0] > p[2];
}

/* Card fill 1, 0.2, 0.7 — not cube red, not teal clear. */
static int is_card(const uint8_t *p)
{
    return p[0] > 150 && p[2] > 100 && (int)p[0] > (int)p[1] + 80;
}

/* Second card 0.2, 0.9, 0.3. */
static int is_green(const uint8_t *p)
{
    return p[1] > 150 && (int)p[1] > (int)p[0] + 80 && (int)p[1] > (int)p[2] + 80;
}

static unsigned count_pred(uint8_t *pix, int x0, int y0, int x1, int y1,
                           int (*pred)(const uint8_t *))
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
    struct wp_card_geom g;
    struct wp_draw_list list;
    float ortho[16];
    float tl[3], bl[3], br[3], tr[3];
    float magenta[4] = { 1.0f, 0.2f, 0.7f, 1.0f };
    int ret;

    expect(wp_card_cpu(0, 0, 1, 1, NULL) == -EINVAL, "null geom");
    expect(wp_card_cpu(0, 0, 0, 1, &g) == -EINVAL, "zero width");
    expect(wp_card_cpu(0, 0, 1, 0, &g) == -EINVAL, "zero height");
    expect(wp_card_cpu(0, 0, -4, 8, &g) == -EINVAL, "negative width");

    ret = wp_card_cpu(0, 0, 1, 1, &g);
    expect(ret == 0, "unit quad");
    expect(g.x0 == 0 && g.y0 == 0 && g.x1 == 1 && g.y1 == 1, "unit AABB");

    ret = wp_card_cpu(16, 24, 40, 30, &g);
    expect(ret == 0, "pixel rect");
    expect(g.x0 == 16 && g.y0 == 24 && g.x1 == 56 && g.y1 == 54, "AABB is x,y,x+w,y+h");
    expect(g.v[0].x == 16 && g.v[0].y == 24, "vert 0 is top-left");
    expect(g.v[1].x == 16 && g.v[1].y == 54, "vert 1 is bottom-left");
    expect(g.v[2].x == 56 && g.v[2].y == 54, "vert 2 is bottom-right");
    expect(g.v[3].x == 56 && g.v[3].y == 24, "vert 3 is top-right");
    expect(g.idx[0] == 0 && g.idx[1] == 1 && g.idx[2] == 2, "first tri TL-BL-BR");
    expect(g.idx[3] == 0 && g.idx[4] == 2 && g.idx[5] == 3, "second tri TL-BR-TR");

    tl[0] = g.v[0].x;
    tl[1] = g.v[0].y;
    tl[2] = 0;
    bl[0] = g.v[1].x;
    bl[1] = g.v[1].y;
    bl[2] = 0;
    br[0] = g.v[2].x;
    br[1] = g.v[2].y;
    br[2] = 0;
    tr[0] = g.v[3].x;
    tr[1] = g.v[3].y;
    tr[2] = 0;
    wp_mat4_ortho_pixel(ortho, 1280.0f, 720.0f);
    expect(wp_triangle_front_facing(ortho, tl, bl, br),
           "factory TL-BL-BR is a raster front face (not culled)");
    expect(!wp_triangle_front_facing(ortho, tl, tr, br),
           "clockwise TL-TR-BR is a back face (CULL_BACK would drop it)");

    wp_draw_list_init(&list);
    {
        struct wp_rect r = { 16, 24, 40, 30 };
        struct wp_rect bad = { 16, 24, 0, 30 };
        expect(!wp_rect_ok(NULL) && !wp_rect_ok(&bad), "wp_rect_ok rejects empty");
        expect(wp_rect_ok(&r), "wp_rect_ok on a real rect");
        expect(wp_draw_list_push_card(NULL, &r, magenta) == -EINVAL, "push_card null list");
        expect(wp_draw_list_push_card(&list, NULL, magenta) == -EINVAL, "push_card null rect");
        expect(wp_draw_list_push_card(&list, &bad, magenta) == -EINVAL, "push_card zero width");
        expect(wp_draw_list_push_card(&list, &r, NULL) == -EINVAL, "push_card null rgba");
        ret = wp_draw_list_push_card(&list, &r, magenta);
        expect(ret == 0 && wp_draw_list_count_kind(&list, WP_DRAW_CARD) == 1, "list accepts a card rect");
        magenta[0] = 0;
        r.w = 99;
        expect(list.items[0].card.rgba[0] > 0.5f, "list copied rgba");
        expect(list.items[0].card.rect.w == 40, "list copied the rect (caller can change after push)");
        {
            struct wp_rect b = { 80, 8, 32, 24 };
            float green[4] = { 0.2f, 0.9f, 0.3f, 1.0f };
            ret = wp_draw_list_push_card(&list, &b, green);
            expect(ret == 0 && wp_draw_list_count_kind(&list, WP_DRAW_CARD) == 2,
                   "two card rects on the list");
        }
    }
    wp_draw_list_destroy(&list);
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
    struct wp_card_geom geom;
    struct wp_camera cam;
    struct wp_present_frame f;
    struct wp_rect ra, rb;
    float model[16];
    float fill[4] = { 1.0f, 0.2f, 0.7f, 1.0f };
    float green[4] = { 0.2f, 0.9f, 0.3f, 1.0f };
    uint8_t *pix;
    const uint8_t *c;
    unsigned n_card = 0, n_far = 0, n_a, n_b, n_wrong, presented = 0;
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
    memset(&geom, 0, sizeof(geom));
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

    ret = wp_card_cpu(16, 24, 40, 30, &geom);
    expect(ret == 0, "tessellate 40x30 card at (16,24)");
    wp_camera_default(&cam);
    cam.eye[0] = 0.0f;
    cam.eye[1] = 0.0f;
    cam.eye[2] = 2.5f;

    ra = (struct wp_rect){ 16, 24, 40, 30 };
    ret = wp_draw_list_push_lit(&list, &mesh, &cam, model);
    expect(ret == 0, "push cube");
    ret = wp_draw_list_push_card(&list, &ra, fill);
    expect(ret == 0, "push card from wp_rect");

    ret = render_list(&p->device, &pass, &list, &lit, &card, pix);
    expect(ret == 0, "GPU readback of cube + one overlay card");
    if (ret == 0) {
        c = px_at(pix, RW / 2, RH / 2);
        printf("      cube center RGBA %u %u %u %u\n", c[0], c[1], c[2], c[3]);
        expect(is_plus_z(c), "card does not cover the cube (not a fullscreen panel shader)");
        n_card = count_pred(pix, 16, 24, 56, 54, is_card);
        n_far = count_pred(pix, RW - 24, 0, RW, 12, is_card);
        printf("      card AABB covered %u / 1200  far %u\n", n_card, n_far);
        expect(n_card > 1000, "card fill covers its AABB (not culled, not empty)");
        expect(n_far == 0, "no card ink in the opposite corner");
        c = px_at(pix, 36, 39);
        printf("      card interior RGBA %u %u %u %u\n", c[0], c[1], c[2], c[3]);
        expect(is_card(c), "card interior is the factory color");
    }

    /* Two cards as data. Different colors. Then shrink B.w; GPU follows. */
    ra = (struct wp_rect){ 8, 8, 32, 24 };
    rb = (struct wp_rect){ 80, 8, 32, 24 };
    wp_draw_list_clear(&list);
    ret = wp_draw_list_push_lit(&list, &mesh, &cam, model);
    expect(ret == 0, "two-card list: cube");
    ret = wp_draw_list_push_card(&list, &ra, fill);
    expect(ret == 0, "two-card list: magenta A");
    ret = wp_draw_list_push_card(&list, &rb, green);
    expect(ret == 0, "two-card list: green B");
    expect(wp_draw_list_count_kind(&list, WP_DRAW_CARD) == 2, "two card items");

    memset(pix, 0, (size_t)RW * RH * 4);
    ret = render_list(&p->device, &pass, &list, &lit, &card, pix);
    expect(ret == 0, "GPU readback of two cards");
    if (ret == 0) {
        c = px_at(pix, RW / 2, RH / 2);
        expect(is_plus_z(c), "two cards still leave the cube center");
        n_a = count_pred(pix, 8, 8, 40, 32, is_card);
        n_wrong = count_pred(pix, 8, 8, 40, 32, is_green);
        n_b = count_pred(pix, 80, 8, 112, 32, is_green);
        n_wrong += count_pred(pix, 80, 8, 112, 32, is_card);
        printf("      A magenta %u  B green %u  cross %u\n", n_a, n_b, n_wrong);
        expect(n_a > 700, "card A fills its rect");
        expect(n_b > 700, "card B fills its rect");
        expect(n_wrong == 0, "second card did not paint the first (and vice versa)");
    }

    rb.w = 16;
    wp_draw_list_clear(&list);
    ret = wp_draw_list_push_lit(&list, &mesh, &cam, model);
    expect(ret == 0, "re-push cube");
    ret = wp_draw_list_push_card(&list, &ra, fill);
    expect(ret == 0, "re-push A");
    ret = wp_draw_list_push_card(&list, &rb, green);
    expect(ret == 0, "re-push B with w=16 (CPU width change)");
    memset(pix, 0, (size_t)RW * RH * 4);
    ret = render_list(&p->device, &pass, &list, &lit, &card, pix);
    expect(ret == 0, "GPU readback after shrinking B");
    if (ret == 0) {
        n_a = count_pred(pix, 8, 8, 40, 32, is_card);
        n_b = count_pred(pix, 80, 8, 96, 32, is_green);
        n_wrong = count_pred(pix, 96, 8, 112, 32, is_green);
        printf("      after shrink  A %u  B %u  abandoned strip %u\n", n_a, n_b, n_wrong);
        expect(n_a > 700, "card A unmoved after B's width changed");
        expect(n_b > 350, "card B fills the new (narrower) AABB");
        expect(n_wrong == 0, "pixels B no longer owns are empty of B (GPU followed CPU w)");
        c = px_at(pix, RW / 2, RH / 2);
        expect(is_plus_z(c), "cube center still +Z after the width change");
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
    printf("      card frames %u  %ux%u\n", presented, p->sc.width, p->sc.height);
    expect(presented >= 2, "presented frames with the card on the list");

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

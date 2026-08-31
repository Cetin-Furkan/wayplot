#define _GNU_SOURCE
#include "engine/doc.h"
#include "engine/draw.h"
#include "engine/hit.h"
#include "engine/present.h"
#include "engine/view.h"
#include "helper/math3d.h"
#include "renderer/camera.h"
#include "renderer/card.h"
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

static int is_white(const uint8_t *p)
{
    return p[0] > 180 && p[1] > 180 && p[2] > 180;
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
    struct wp_doc doc;
    struct wp_hit_stack hits;
    struct wp_hit_item it;
    struct wp_draw_list list;
    struct wp_rect ra = { 8, 8, 32, 24 };
    struct wp_rect rb = { 80, 8, 32, 24 };
    struct wp_rect bad = { 8, 8, 0, 24 };
    float magenta[4] = { 1.0f, 0.2f, 0.7f, 1.0f };
    float green[4] = { 0.2f, 0.9f, 0.3f, 1.0f };
    struct wp_mesh *fake = (struct wp_mesh *)(uintptr_t)0x1000;
    struct wp_mesh *keep;
    uint32_t i;
    int ret;

    wp_doc_init(NULL);
    wp_doc_clear(NULL);
    wp_doc_destroy(NULL);
    expect(wp_doc_nmesh(NULL) == 0 && wp_doc_ncards(NULL) == 0, "null doc counts are 0");
    expect(wp_doc_add_mesh(NULL, fake) == -EINVAL, "add_mesh null doc");
    expect(wp_doc_add_card(NULL, ra, magenta) == -EINVAL, "add_card null doc");

    wp_doc_init(&doc);
    expect(wp_doc_nmesh(&doc) == 0 && wp_doc_ncards(&doc) == 0, "init is empty");
    expect(wp_doc_add_mesh(&doc, NULL) == -EINVAL, "null mesh rejected");
    expect(wp_doc_add_card(&doc, ra, NULL) == -EINVAL, "null rgba rejected");
    expect(wp_doc_add_card(&doc, bad, magenta) == -EINVAL, "empty rect rejected");
    expect(wp_doc_mesh(&doc, 0) == NULL && wp_doc_card(&doc, 0) == NULL, "empty getters");

    ret = wp_doc_add_card(&doc, ra, magenta);
    expect(ret == 0 && wp_doc_ncards(&doc) == 1, "add card A");
    magenta[0] = 0.0f;
    ra.w = 99;
    expect(doc.cards[0].rgba[0] > 0.5f, "doc copied rgba");
    expect(doc.cards[0].rect.w == 32, "doc copied the rect");
    ret = wp_doc_add_card(&doc, rb, green);
    expect(ret == 0 && wp_doc_ncards(&doc) == 2, "add card B");
    expect(wp_doc_caption(&doc, 0)[0] == 0, "caption starts empty");
    expect(wp_doc_set_caption(&doc, 0, "DEM 32x32") == 0, "set caption");
    expect(strcmp(wp_doc_caption(&doc, 0), "DEM 32x32") == 0, "caption copied");
    expect(wp_doc_set_caption(&doc, 0, "") == 0 && wp_doc_caption(&doc, 0)[0] == 0, "empty caption");
    expect(wp_doc_set_caption(&doc, 99, "x") == -EINVAL, "caption bad index");
    {
        char longc[200];
        memset(longc, 'A', 199);
        longc[199] = 0;
        expect(wp_doc_set_caption(&doc, 0, longc) == 0, "long caption accepted");
        expect(strlen(wp_doc_caption(&doc, 0)) == WP_DOC_CAPTION - 1, "caption truncated to cap");
        expect(wp_doc_set_caption(&doc, 0, "") == 0, "clear caption");
    }
    ret = wp_doc_add_mesh(&doc, fake);
    expect(ret == 0 && wp_doc_nmesh(&doc) == 1 && wp_doc_mesh(&doc, 0) == fake,
           "add mesh pointer");
    keep = wp_doc_mesh(&doc, 0);

    wp_hit_clear(&hits);
    expect(wp_doc_fill_hits(NULL, &hits) == -EINVAL, "fill_hits null doc");
    expect(wp_doc_fill_hits(&doc, NULL) == -EINVAL, "fill_hits null stack");
    ret = wp_doc_fill_hits(&doc, &hits);
    expect(ret == 0 && hits.count == 2, "fill_hits two cards");
    expect(hits.items[0].id == 1 && hits.items[1].id == 2, "hit id is index+1");
    expect(wp_hit_pick(&hits, 12, 12, &it) == 1 && it.id == 1, "pick A from doc hits");
    expect(wp_hit_pick(&hits, 90, 16, &it) == 1 && it.id == 2, "pick B from doc hits");
    expect(wp_hit_pick(&hits, 50, 16, NULL) == 0, "gap is still a miss");

    expect(wp_doc_apply_drag(NULL, 2, rb) == -EINVAL, "apply_drag null doc");
    expect(wp_doc_apply_drag(&doc, 0, rb) == 0, "hit_id 0 is a no-op");
    expect(doc.cards[1].rect.x == 80, "no-op did not move B");
    expect(wp_doc_apply_drag(&doc, 99, rb) == -EINVAL, "unknown hit id");
    {
        struct wp_rect drag = { 96, 8, 32, 24 };
        expect(wp_doc_apply_drag(&doc, 2, drag) == 0, "drag B");
        expect(doc.cards[1].rect.x == 96 && doc.cards[1].rect.y == 8, "B x,y follow drag");
        expect(doc.cards[1].rect.w == 32 && doc.cards[1].rect.h == 24, "drag does not change w/h");
        expect(doc.cards[0].rect.x == 8, "A unmoved by B's drag");
    }

    wp_draw_list_init(&list);
    expect(wp_doc_push_cards(NULL, &list, 1.0f) == -EINVAL, "push_cards null doc");
    expect(wp_doc_push_cards(&doc, NULL, 1.0f) == -EINVAL, "push_cards null list");
    ret = wp_doc_push_cards(&doc, &list, 1.0f);
    expect(ret == 0 && wp_draw_list_count_kind(&list, WP_DRAW_CARD) == 2, "push two cards onto the list");
    expect(list.items[1].card.rect.x == 96, "list saw B after drag");
    wp_draw_list_clear(&list);
    ret = wp_doc_push_cards(&doc, &list, 2.0f);
    expect(ret == 0 && list.items[0].card.rect.w == 64, "push_cards applies scale");
    wp_draw_list_destroy(&list);

    doc.cards[1].rect.w = 16;
    expect(doc.cards[1].rect.w == 16, "caller can change w on the doc");
    expect(wp_doc_set_card_rect(&doc, 1, bad) == -EINVAL, "set_card_rect rejects empty");
    expect(doc.cards[1].rect.w == 16, "failed set left w");

    wp_doc_destroy(&doc);
    expect(wp_doc_nmesh(&doc) == 0 && wp_doc_ncards(&doc) == 0, "destroy drops slots");
    expect(keep == fake, "destroy does not free the mesh pointer");

    wp_doc_init(&doc);
    for (i = 0; i < WP_DOC_MAX_CARD; i++) {
        struct wp_rect r = { (float)(i % 8) * 16.f, (float)(i / 8) * 8.f, 16, 8 };
        if (wp_doc_add_card(&doc, r, magenta) != 0) {
            g_fail++;
            printf("FAIL  fill cards at %u\n", i);
            break;
        }
    }
    expect(wp_doc_ncards(&doc) == WP_DOC_MAX_CARD, "cards fill to cap");
    expect(wp_doc_add_card(&doc, ra, magenta) == -ENOSPC, "card cap is ENOSPC");
    {
        uint32_t n = 200000, k;
        uint64_t t0, t1;
        t0 = now_ns();
        for (k = 0; k < n; k++) {
            wp_hit_clear(&hits);
            (void)wp_doc_fill_hits(&doc, &hits);
        }
        t1 = now_ns();
        printf("      fill_hits %u cards × %u  %.2f ns/fill\n", WP_DOC_MAX_CARD, n,
               (double)(t1 - t0) / (double)n);
        expect(hits.count == WP_DOC_MAX_CARD, "fill_hits packed the stack");
    }
    wp_doc_clear(&doc);
    expect(wp_doc_ncards(&doc) == 0, "clear drops cards");
    for (i = 0; i < WP_DOC_MAX_MESH; i++) {
        if (wp_doc_add_mesh(&doc, fake) != 0) {
            g_fail++;
            printf("FAIL  fill meshes at %u\n", i);
            break;
        }
    }
    expect(wp_doc_nmesh(&doc) == WP_DOC_MAX_MESH, "meshes fill to cap");
    expect(wp_doc_add_mesh(&doc, fake) == -ENOSPC, "mesh cap is ENOSPC");
    wp_doc_destroy(&doc);
}

static int render_doc(struct wp_device *d, struct wp_pass *pass, struct wp_lit *lit,
                      struct wp_card *card, struct wp_text *txt, struct wp_draw_list *list,
                      struct wp_view *view, const struct wp_doc *doc, const float model[16],
                      uint8_t *pixels)
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
    uint32_t vw, vh, idx, mi;
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
    if (wp_view_bind(view, cmd, 1, RW, RH) < 0)
        goto out;
    if (wp_view_pixels(view, 1, RW, RH, &vx, &vy, &vw, &vh) < 0)
        goto out;
    for (mi = 0; mi < wp_doc_nmesh(doc); mi++) {
        struct wp_mesh *m = wp_doc_mesh(doc, mi);
        if (!m || !m->index_count)
            continue;
        wp_lit_draw(lit, cmd, vw, vh, 0, m, &view->cam, model);
    }
    wp_pass_opaque_end(cmd);
    wp_pass_overlay_begin(pass, cmd, iv, RW, RH);
    wp_draw_list_record_overlay(list, txt, card, cmd, RW, RH, 0);
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
    if (iv)
        vkDestroyImageView(d->device, iv, NULL);
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
    struct wp_doc doc;
    struct wp_mesh mesh;
    struct wp_lit lit;
    struct wp_card card;
    struct wp_view view;
    struct wp_present_frame f;
    struct wp_rect ra, rb;
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
    wp_doc_init(&doc);
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
    wp_view_init(&view);
    view.rect = (struct wp_rect){ 0, 0, RW, RH };
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
    ret = wp_mesh_cube(&p->device, &mesh);
    expect(ret == 0, "mesh");
    ret = wp_doc_add_mesh(&doc, &mesh);
    expect(ret == 0 && wp_doc_mesh(&doc, 0) == &mesh, "doc holds the cube pointer");
    ret = wp_lit_init(&lit, &p->device, VK_FORMAT_R8G8B8A8_UNORM);
    expect(ret == 0, "lit");
    if (ret < 0)
        goto done;
    ret = wp_card_init(&card, &p->device, VK_FORMAT_R8G8B8A8_UNORM);
    expect(ret == 0, "card pipeline");
    if (ret < 0)
        goto done;

    ra = (struct wp_rect){ 8, 8, 32, 24 };
    rb = (struct wp_rect){ 80, 8, 32, 24 };
    expect(wp_doc_add_card(&doc, ra, fill) == 0, "doc card A");
    expect(wp_doc_add_card(&doc, rb, green) == 0, "doc card B");

    wp_draw_list_clear(&list);
    ret = wp_doc_push_cards(&doc, &list, 1.0f);
    expect(ret == 0 && wp_draw_list_count_kind(&list, WP_DRAW_CARD) == 2, "GPU list from doc");

    memset(pix, 0, (size_t)RW * RH * 4);
    ret = render_doc(&p->device, &pass, &lit, &card, NULL, &list, &view, &doc, model, pix);
    expect(ret == 0, "GPU readback of doc cube + two cards");
    if (ret == 0) {
        c = px_at(pix, RW / 2, RH / 2);
        printf("      cube center RGBA %u %u %u %u\n", c[0], c[1], c[2], c[3]);
        expect(is_plus_z(c), "cube +Z in the view (cards are not a fullscreen panel)");
        n_a = count_pred(pix, 8, 8, 40, 32, is_card);
        n_wrong = count_pred(pix, 8, 8, 40, 32, is_green);
        n_b = count_pred(pix, 80, 8, 112, 32, is_green);
        n_wrong += count_pred(pix, 80, 8, 112, 32, is_card);
        printf("      A magenta %u  B green %u  cross %u\n", n_a, n_b, n_wrong);
        expect(n_a > 700, "card A fills its rect");
        expect(n_b > 700, "card B fills its rect");
        expect(n_wrong == 0, "second card did not paint the first (and vice versa)");
    }

    doc.cards[1].rect.w = 16;
    wp_draw_list_clear(&list);
    ret = wp_doc_push_cards(&doc, &list, 1.0f);
    expect(ret == 0, "re-push after shrinking B.w on the doc");
    memset(pix, 0, (size_t)RW * RH * 4);
    ret = render_doc(&p->device, &pass, &lit, &card, NULL, &list, &view, &doc, model, pix);
    expect(ret == 0, "GPU readback after shrinking B on the doc");
    if (ret == 0) {
        n_a = count_pred(pix, 8, 8, 40, 32, is_card);
        n_b = count_pred(pix, 80, 8, 96, 32, is_green);
        n_wrong = count_pred(pix, 96, 8, 112, 32, is_green);
        printf("      after shrink  A %u  B %u  abandoned strip %u\n", n_a, n_b, n_wrong);
        expect(n_a > 700, "card A unmoved after B's width changed on the doc");
        expect(n_b > 350, "card B fills the new (narrower) AABB");
        expect(n_wrong == 0, "pixels B no longer owns are empty of B (GPU followed the doc)");
        c = px_at(pix, RW / 2, RH / 2);
        expect(is_plus_z(c), "cube center still +Z after the width change");
    }

    {
        struct wp_font font;
        struct wp_text txt;
        struct wp_text_geom geom;
        float white[4] = { 0.95f, 0.95f, 0.95f, 1.0f };
        unsigned n_ink, n_empty;
        memset(&font, 0, sizeof(font));
        memset(&txt, 0, sizeof(txt));
        memset(&geom, 0, sizeof(geom));
        ret = wp_font_open_default(&font, 16.0f);
        expect(ret == 0, "caption font");
        if (ret == 0)
            ret = wp_font_upload(&font, &p->device);
        expect(ret == 0, "caption font upload");
        if (ret == 0)
            ret = wp_text_init(&txt, &p->device, VK_FORMAT_R8G8B8A8_UNORM);
        expect(ret == 0, "caption text pipeline");
        if (ret == 0) {
            expect(wp_doc_set_card_rect(&doc, 0, (struct wp_rect){ 8, 8, 112, 48 }) == 0,
                   "widen card A for a caption");
            expect(wp_doc_set_caption(&doc, 0, "HHHH") == 0, "caption HHHH");
            (void)wp_text_layout(&font, wp_doc_caption(&doc, 0), 12.0f, 12.0f, &geom);
            wp_draw_list_clear(&list);
            expect(wp_doc_push_cards(&doc, &list, 1.0f) == 0, "cards for caption");
            expect(wp_draw_list_push_text(&list, &font, &geom, white) == 0, "push caption glyphs");
            memset(pix, 0, (size_t)RW * RH * 4);
            ret = render_doc(&p->device, &pass, &lit, &card, &txt, &list, &view, &doc, model, pix);
            expect(ret == 0, "GPU readback of card caption");
            n_ink = count_pred(pix, 8, 8, 120, 56, is_white);
            printf("      caption ink %u\n", n_ink);
            expect(n_ink > 40, "white glyphs sit on the card");
            c = px_at(pix, RW / 2, RH / 2);
            expect(is_plus_z(c), "caption is not a fullscreen panel");
            expect(wp_doc_set_caption(&doc, 0, "") == 0, "clear caption");
            wp_text_geom_free(&geom);
            wp_draw_list_clear(&list);
            expect(wp_doc_push_cards(&doc, &list, 1.0f) == 0, "cards after clearing caption");
            memset(pix, 0, (size_t)RW * RH * 4);
            ret = render_doc(&p->device, &pass, &lit, &card, &txt, &list, &view, &doc, model, pix);
            expect(ret == 0, "GPU readback with empty caption");
            n_empty = count_pred(pix, 8, 8, 120, 56, is_white);
            printf("      empty caption ink %u\n", n_empty);
            expect(n_empty == 0, "clearing the caption removes the glyphs");
        }
        wp_text_geom_free(&geom);
        wp_text_destroy(&txt);
        wp_font_destroy(&font);
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

    wp_draw_list_clear(&list);
    ret = wp_draw_list_push_lit(&list, wp_doc_mesh(&doc, 0), &view.cam, model);
    expect(ret == 0, "present list: cube from doc");
    ret = wp_doc_push_cards(&doc, &list, 1.0f);
    expect(ret == 0, "present list: cards from doc");

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
    printf("      doc frames %u  %ux%u\n", presented, p->sc.width, p->sc.height);
    expect(presented >= 2, "presented frames with the doc on the list");

done:
    wp_draw_list_destroy(&list);
    wp_doc_destroy(&doc);
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

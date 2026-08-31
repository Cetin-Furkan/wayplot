#define _GNU_SOURCE
#include "renderer/font.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ft2build.h>
#include FT_FREETYPE_H

static const char *const default_paths[] = {
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
};

static int blit_bitmap(uint8_t *atlas, uint32_t aw, uint32_t ah,
                       uint32_t dx, uint32_t dy, const FT_Bitmap *bm)
{
    uint32_t y, x;

    if (bm->pixel_mode != FT_PIXEL_MODE_GRAY)
        return -ENOTSUP;
    for (y = 0; y < bm->rows; y++) {
        if (dy + y >= ah)
            return -ENOMEM;
        const uint8_t *src = bm->buffer + (int)y * bm->pitch;
        uint8_t *dst = atlas + (size_t)(dy + y) * aw + dx;
        uint32_t w = bm->width;
        if (dx + w > aw)
            return -ENOMEM;
        for (x = 0; x < w; x++)
            dst[x] = src[x];
    }
    return 0;
}

static int pack_and_metrics(struct wp_font *f, FT_Face face)
{
    const uint32_t aw = 512, ah = 512;
    uint32_t x = 1, y = 1, row_h = 0;
    uint32_t i;
    int ret;

    f->atlas_w = aw;
    f->atlas_h = ah;
    f->atlas = calloc((size_t)aw * ah, 1);
    if (!f->atlas)
        return -ENOMEM;

    for (i = 0; i < WP_FONT_N + 1; i++) {
        uint32_t cp = (i < WP_FONT_N) ? (WP_FONT_CP0 + i) : (uint32_t)'?';
        struct wp_glyph *g = (i < WP_FONT_N) ? &f->glyph[i] : &f->missing;
        FT_UInt idx = FT_Get_Char_Index(face, cp);
        uint32_t gw, gh;

        memset(g, 0, sizeof(*g));
        if (FT_Load_Glyph(face, idx, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) != 0)
            continue;
        gw = face->glyph->bitmap.width;
        gh = face->glyph->bitmap.rows;
        if (gw > 0 && gh > 0) {
            if (x + gw + 1 >= aw) {
                x = 1;
                y += row_h + 1;
                row_h = 0;
            }
            if (y + gh + 1 >= ah)
                return -ENOMEM;
            ret = blit_bitmap(f->atlas, aw, ah, x, y, &face->glyph->bitmap);
            if (ret < 0)
                return ret;
            g->u0 = (float)x / (float)aw;
            g->v0 = (float)y / (float)ah;
            g->u1 = (float)(x + gw) / (float)aw;
            g->v1 = (float)(y + gh) / (float)ah;
            x += gw + 1;
            if (gh > row_h)
                row_h = gh;
        }
        g->advance = (float)face->glyph->advance.x / 64.0f;
        g->left = (float)face->glyph->bitmap_left;
        g->top = (float)face->glyph->bitmap_top;
        g->w = (float)gw;
        g->h = (float)gh;
        g->present = 1;
    }

    if (FT_HAS_KERNING(face)) {
        uint32_t a, b;
        for (a = 0; a < WP_FONT_N; a++) {
            for (b = 0; b < WP_FONT_N; b++) {
                FT_Vector k;
                FT_UInt ia = FT_Get_Char_Index(face, WP_FONT_CP0 + a);
                FT_UInt ib = FT_Get_Char_Index(face, WP_FONT_CP0 + b);
                if (FT_Get_Kerning(face, ia, ib, FT_KERNING_DEFAULT, &k) != 0)
                    continue;
                if (k.x > 32767)
                    k.x = 32767;
                if (k.x < -32768)
                    k.x = -32768;
                f->kern[a][b] = (int16_t)k.x;
            }
        }
    }
    return 0;
}

int wp_font_open(struct wp_font *f, const char *path, float size_px)
{
    FT_Library lib = NULL;
    FT_Face face = NULL;
    int ret;

    if (!f || !path || size_px < 4.0f)
        return -EINVAL;
    memset(f, 0, sizeof(*f));
    f->size_px = size_px;
    if (FT_Init_FreeType(&lib) != 0)
        return -EIO;
    if (FT_New_Face(lib, path, 0, &face) != 0) {
        FT_Done_FreeType(lib);
        return -ENOENT;
    }
    if (FT_Set_Pixel_Sizes(face, 0, (FT_UInt)size_px) != 0) {
        FT_Done_Face(face);
        FT_Done_FreeType(lib);
        return -EINVAL;
    }
    f->ascent = (float)face->size->metrics.ascender / 64.0f;
    f->descent = -(float)face->size->metrics.descender / 64.0f;
    f->line_height = (float)face->size->metrics.height / 64.0f;
    if (f->line_height < 1.0f)
        f->line_height = size_px * 1.25f;
    ret = pack_and_metrics(f, face);
    FT_Done_Face(face);
    FT_Done_FreeType(lib);
    if (ret < 0) {
        free(f->atlas);
        memset(f, 0, sizeof(*f));
        return ret;
    }
    return 0;
}

int wp_font_open_default(struct wp_font *f, float size_px)
{
    uint32_t i;
    int ret = -ENOENT;

    for (i = 0; i < sizeof(default_paths) / sizeof(default_paths[0]); i++) {
        if (access(default_paths[i], R_OK) != 0)
            continue;
        ret = wp_font_open(f, default_paths[i], size_px);
        if (ret == 0)
            return 0;
    }
    return ret;
}

const struct wp_glyph *wp_font_glyph(const struct wp_font *f, uint32_t cp)
{
    if (!f)
        return NULL;
    if (cp >= WP_FONT_CP0 && cp < WP_FONT_CP0 + WP_FONT_N &&
        f->glyph[cp - WP_FONT_CP0].present)
        return &f->glyph[cp - WP_FONT_CP0];
    return &f->missing;
}

float wp_font_kern(const struct wp_font *f, uint32_t a, uint32_t b)
{
    if (!f)
        return 0.0f;
    if (a < WP_FONT_CP0 || a >= WP_FONT_CP0 + WP_FONT_N)
        return 0.0f;
    if (b < WP_FONT_CP0 || b >= WP_FONT_CP0 + WP_FONT_N)
        return 0.0f;
    return (float)f->kern[a - WP_FONT_CP0][b - WP_FONT_CP0] / 64.0f;
}

int wp_font_upload(struct wp_font *f, struct wp_device *d)
{
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8_UNORM,
        .extent = { 0, 0, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_HOST_TRANSFER_BIT,
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
    VkHostImageLayoutTransitionInfo tr = {
        .sType = VK_STRUCTURE_TYPE_HOST_IMAGE_LAYOUT_TRANSITION_INFO,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    VkMemoryToImageCopy region = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_TO_IMAGE_COPY,
        .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .imageExtent = { 0, 0, 1 },
    };
    VkCopyMemoryToImageInfo cpy = {
        .sType = VK_STRUCTURE_TYPE_COPY_MEMORY_TO_IMAGE_INFO,
        .dstImageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .regionCount = 1,
        .pRegions = &region,
    };
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = 1.0f,
    };
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8_UNORM,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    uint32_t idx;

    if (!f || !d || !f->atlas)
        return -EINVAL;
    if (!d->host_image_copy)
        return -ENOTSUP;
    if (f->image)
        return 0;
    ici.extent.width = f->atlas_w;
    ici.extent.height = f->atlas_h;
    region.imageExtent.width = f->atlas_w;
    region.imageExtent.height = f->atlas_h;
    region.pHostPointer = f->atlas;
    vkGetDeviceImageMemoryRequirements(d->device, &q, &memreq);
    if (vkCreateImage(d->device, &ici, NULL, &f->image) != VK_SUCCESS) {
        ici.tiling = VK_IMAGE_TILING_LINEAR;
        q.pCreateInfo = &ici;
        vkGetDeviceImageMemoryRequirements(d->device, &q, &memreq);
        if (vkCreateImage(d->device, &ici, NULL, &f->image) != VK_SUCCESS)
            return -EIO;
    }
    vkGetPhysicalDeviceMemoryProperties(d->phy, &mprops);
    idx = wp_find_memory_type(&mprops, memreq.memoryRequirements.memoryTypeBits,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (idx == UINT32_MAX)
        idx = wp_find_memory_type(&mprops, memreq.memoryRequirements.memoryTypeBits, 0);
    if (idx == UINT32_MAX)
        return -ENOMEM;
    ai.allocationSize = memreq.memoryRequirements.size;
    ai.memoryTypeIndex = idx;
    if (vkAllocateMemory(d->device, &ai, NULL, &f->mem) != VK_SUCCESS)
        return -ENOMEM;
    if (vkBindImageMemory(d->device, f->image, f->mem, 0) != VK_SUCCESS)
        return -EIO;
    tr.image = f->image;
    if (vkTransitionImageLayout(d->device, 1, &tr) != VK_SUCCESS)
        return -EIO;
    cpy.dstImage = f->image;
    if (vkCopyMemoryToImage(d->device, &cpy) != VK_SUCCESS)
        return -EIO;
    tr.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    tr.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (vkTransitionImageLayout(d->device, 1, &tr) == VK_SUCCESS)
        f->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    else
        f->layout = VK_IMAGE_LAYOUT_GENERAL;
    vci.image = f->image;
    if (vkCreateImageView(d->device, &vci, NULL, &f->view) != VK_SUCCESS)
        return -EIO;
    if (vkCreateSampler(d->device, &sci, NULL, &f->sampler) != VK_SUCCESS)
        return -EIO;
    f->dev = d;
    return 0;
}

void wp_font_destroy(struct wp_font *f)
{
    if (!f)
        return;
    if (f->dev) {
        vkDeviceWaitIdle(f->dev->device);
        if (f->sampler)
            vkDestroySampler(f->dev->device, f->sampler, NULL);
        if (f->view)
            vkDestroyImageView(f->dev->device, f->view, NULL);
        if (f->image)
            vkDestroyImage(f->dev->device, f->image, NULL);
        if (f->mem)
            vkFreeMemory(f->dev->device, f->mem, NULL);
    }
    free(f->atlas);
    memset(f, 0, sizeof(*f));
}

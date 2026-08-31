#define _GNU_SOURCE
#include "renderer/image.h"

#include "helper/math3d.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void wp_image_free(struct wp_image *im)
{
    if (!im)
        return;
    free(im->rgba);
    memset(im, 0, sizeof(*im));
}

static int skip_ppm(const unsigned char **pp, const unsigned char *end)
{
    const unsigned char *p = *pp;

    for (;;) {
        while (p < end && isspace(*p))
            p++;
        if (p < end && *p == '#') {
            while (p < end && *p != '\n')
                p++;
            continue;
        }
        break;
    }
    *pp = p;
    return p < end ? 0 : -EINVAL;
}

static int parse_u32(const unsigned char **pp, const unsigned char *end, uint32_t *out)
{
    unsigned long v = 0;
    const unsigned char *p;

    if (skip_ppm(pp, end) < 0)
        return -EINVAL;
    p = *pp;
    if (p >= end || !isdigit(*p))
        return -EINVAL;
    while (p < end && isdigit(*p)) {
        v = v * 10u + (unsigned long)(*p - '0');
        if (v > 65535ul)
            return -E2BIG;
        p++;
    }
    *out = (uint32_t)v;
    *pp = p;
    return 0;
}

int wp_image_parse_ppm(const void *data, size_t len, struct wp_image *out)
{
    const unsigned char *p, *end, *bin;
    uint32_t w, h, maxv, i, n;

    if (!data || !out || len < 8)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    p = data;
    end = p + len;
    if (p[0] != 'P' || p[1] != '6')
        return -EINVAL;
    p += 2;
    if (parse_u32(&p, end, &w) < 0 || parse_u32(&p, end, &h) < 0 || parse_u32(&p, end, &maxv) < 0)
        return -EINVAL;
    if (w < 1 || h < 1 || w > WP_IMAGE_MAX_SIDE || h > WP_IMAGE_MAX_SIDE)
        return -E2BIG;
    if (maxv == 0 || maxv >= 256)
        return -EINVAL;
    if (p >= end || !isspace(*p))
        return -EINVAL;
    p++;
    n = w * h;
    if ((size_t)(end - p) < (size_t)n * 3u)
        return -EINVAL;
    bin = p;
    out->rgba = calloc((size_t)n, 4u);
    if (!out->rgba)
        return -ENOMEM;
    for (i = 0; i < n; i++) {
        out->rgba[i * 4u + 0] = bin[i * 3u + 0];
        out->rgba[i * 4u + 1] = bin[i * 3u + 1];
        out->rgba[i * 4u + 2] = bin[i * 3u + 2];
        out->rgba[i * 4u + 3] = 255;
    }
    out->w = w;
    out->h = h;
    return 0;
}

int wp_image_load(const char *path, struct wp_image *out)
{
    struct stat st;
    unsigned char *buf = NULL;
    size_t got = 0;
    ssize_t nread;
    int fd, ret;

    if (!path || !out)
        return -EINVAL;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    if (fstat(fd, &st) < 0) {
        ret = -errno;
        close(fd);
        return ret;
    }
    if (!S_ISREG(st.st_mode) || st.st_size <= 0) {
        close(fd);
        return -EINVAL;
    }
    if ((uint64_t)st.st_size > (32ull << 20)) {
        close(fd);
        return -EFBIG;
    }
    buf = malloc((size_t)st.st_size);
    if (!buf) {
        close(fd);
        return -ENOMEM;
    }
    while (got < (size_t)st.st_size) {
        nread = read(fd, buf + got, (size_t)st.st_size - got);
        if (nread < 0) {
            ret = -errno;
            free(buf);
            close(fd);
            return ret;
        }
        if (nread == 0)
            break;
        got += (size_t)nread;
    }
    close(fd);
    ret = wp_image_parse_ppm(buf, got, out);
    free(buf);
    return ret;
}

static int emit_xz_image(struct wp_mesh_cpu *out, float x0, float x1, float z0, float z1, float y,
                         int both)
{
    uint32_t ni = both ? 12u : 6u;

    if (!out)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    out->v = calloc(4, sizeof(*out->v));
    out->idx = calloc(ni, sizeof(*out->idx));
    if (!out->v || !out->idx) {
        wp_mesh_cpu_free(out);
        return -ENOMEM;
    }
    out->v[0] = (struct wp_vn_vertex){ x0, y, z0, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f };
    out->v[1] = (struct wp_vn_vertex){ x1, y, z0, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f };
    out->v[2] = (struct wp_vn_vertex){ x0, y, z1, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 1.0f };
    out->v[3] = (struct wp_vn_vertex){ x1, y, z1, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
    out->idx[0] = 0;
    out->idx[1] = 2;
    out->idx[2] = 3;
    out->idx[3] = 0;
    out->idx[4] = 3;
    out->idx[5] = 1;
    if (both) {
        out->idx[6] = 0;
        out->idx[7] = 1;
        out->idx[8] = 3;
        out->idx[9] = 0;
        out->idx[10] = 3;
        out->idx[11] = 2;
    }
    out->nv = 4;
    out->ni = ni;
    return 0;
}

int wp_image_quad(struct wp_mesh_cpu *out)
{
    /* Unit factory for UV tests. Production without a DEM is wp_image_ground. */
    return emit_xz_image(out, -1.0f, 1.0f, -1.0f, 1.0f, 0.0f, 0);
}

int wp_image_ground(const struct wp_aabb *box, struct wp_mesh_cpu *out)
{
    float spanx, spanz, span, pad, r, y;

    if (!wp_aabb_ok(box) || !out)
        return -EINVAL;
    spanx = box->max[0] - box->min[0];
    spanz = box->max[2] - box->min[2];
    if (spanx < 1e-4f)
        spanx = 1.0f;
    if (spanz < 1e-4f)
        spanz = 1.0f;
    span = spanx > spanz ? spanx : spanz;
    pad = 0.12f * span;
    r = wp_aabb_radius(box);
    if (r < 0.15f)
        r = 0.15f;
    y = box->min[1] - 0.04f * r;
    return emit_xz_image(out, box->min[0] - pad, box->max[0] + pad, box->min[2] - pad,
                         box->max[2] + pad, y, 1);
}

int wp_tex_upload(struct wp_device *d, struct wp_tex *t, const struct wp_image *im)
{
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { 1, 1, 1 },
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
        .imageExtent = { 1, 1, 1 },
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
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    uint32_t idx;

    if (!d || !t || !im || !im->rgba || im->w == 0 || im->h == 0)
        return -EINVAL;
    if (!d->host_image_copy)
        return -ENOTSUP;
    memset(t, 0, sizeof(*t));
    t->dev = d;
    ici.extent.width = im->w;
    ici.extent.height = im->h;
    region.imageExtent.width = im->w;
    region.imageExtent.height = im->h;
    region.pHostPointer = im->rgba;
    vkGetDeviceImageMemoryRequirements(d->device, &q, &memreq);
    if (vkCreateImage(d->device, &ici, NULL, &t->image) != VK_SUCCESS) {
        ici.tiling = VK_IMAGE_TILING_LINEAR;
        q.pCreateInfo = &ici;
        vkGetDeviceImageMemoryRequirements(d->device, &q, &memreq);
        if (vkCreateImage(d->device, &ici, NULL, &t->image) != VK_SUCCESS)
            return -EIO;
    }
    vkGetPhysicalDeviceMemoryProperties(d->phy, &mprops);
    idx = wp_find_memory_type(&mprops, memreq.memoryRequirements.memoryTypeBits,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (idx == UINT32_MAX)
        idx = wp_find_memory_type(&mprops, memreq.memoryRequirements.memoryTypeBits, 0);
    if (idx == UINT32_MAX) {
        wp_tex_destroy(t);
        return -ENOMEM;
    }
    ai.allocationSize = memreq.memoryRequirements.size;
    ai.memoryTypeIndex = idx;
    if (vkAllocateMemory(d->device, &ai, NULL, &t->mem) != VK_SUCCESS) {
        wp_tex_destroy(t);
        return -ENOMEM;
    }
    if (vkBindImageMemory(d->device, t->image, t->mem, 0) != VK_SUCCESS) {
        wp_tex_destroy(t);
        return -EIO;
    }
    tr.image = t->image;
    if (vkTransitionImageLayout(d->device, 1, &tr) != VK_SUCCESS) {
        wp_tex_destroy(t);
        return -EIO;
    }
    cpy.dstImage = t->image;
    if (vkCopyMemoryToImage(d->device, &cpy) != VK_SUCCESS) {
        wp_tex_destroy(t);
        return -EIO;
    }
    tr.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    tr.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (vkTransitionImageLayout(d->device, 1, &tr) == VK_SUCCESS)
        t->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    else
        t->layout = VK_IMAGE_LAYOUT_GENERAL;
    vci.image = t->image;
    if (vkCreateImageView(d->device, &vci, NULL, &t->view) != VK_SUCCESS) {
        wp_tex_destroy(t);
        return -EIO;
    }
    if (vkCreateSampler(d->device, &sci, NULL, &t->sampler) != VK_SUCCESS) {
        wp_tex_destroy(t);
        return -EIO;
    }
    t->w = im->w;
    t->h = im->h;
    return 0;
}

void wp_tex_destroy(struct wp_tex *t)
{
    if (!t)
        return;
    if (t->dev && t->dev->device) {
        vkDeviceWaitIdle(t->dev->device);
        if (t->sampler)
            vkDestroySampler(t->dev->device, t->sampler, NULL);
        if (t->view)
            vkDestroyImageView(t->dev->device, t->view, NULL);
        if (t->image)
            vkDestroyImage(t->dev->device, t->image, NULL);
        if (t->mem)
            vkFreeMemory(t->dev->device, t->mem, NULL);
    }
    memset(t, 0, sizeof(*t));
}

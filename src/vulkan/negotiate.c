#define _GNU_SOURCE
#include "vulkan/negotiate.h"

#include <drm/drm_fourcc.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DRM_FORMAT_ARGB8888
#define DRM_FORMAT_ARGB8888 0x34325241
#endif
#ifndef DRM_FORMAT_XRGB8888
#define DRM_FORMAT_XRGB8888 0x34325258
#endif
#ifndef DRM_FORMAT_ABGR8888
#define DRM_FORMAT_ABGR8888 0x34324241
#endif
#ifndef DRM_FORMAT_XBGR8888
#define DRM_FORMAT_XBGR8888 0x34324258
#endif
#ifndef DRM_FORMAT_MOD_INVALID
#define DRM_FORMAT_MOD_INVALID ((1ULL << 56) - 1)
#endif
#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR 0ULL
#endif

static const struct {
    uint32_t drm_format;
    VkFormat vk_format;
    const char *name;
} preferred[] = {
    { DRM_FORMAT_ARGB8888, VK_FORMAT_B8G8R8A8_UNORM, "ARGB8888" },
    { DRM_FORMAT_XRGB8888, VK_FORMAT_B8G8R8A8_UNORM, "XRGB8888" },
    { DRM_FORMAT_ABGR8888, VK_FORMAT_R8G8B8A8_UNORM, "ABGR8888" },
    { DRM_FORMAT_XBGR8888, VK_FORMAT_R8G8B8A8_UNORM, "XBGR8888" },
};

static const char *fmt_name(uint32_t fmt)
{
    size_t i;
    for (i = 0; i < sizeof(preferred) / sizeof(preferred[0]); i++) {
        if (preferred[i].drm_format == fmt)
            return preferred[i].name;
    }
    return "OTHER";
}

void wp_vk_formats_free(struct wp_vk_formats *f)
{
    uint32_t i;
    if (!f)
        return;
    for (i = 0; i < f->n; i++)
        free(f->v[i].modifiers);
    free(f->v);
    memset(f, 0, sizeof(*f));
}

void wp_negotiated_free(struct wp_negotiated *n)
{
    if (!n)
        return;
    free(n->modifiers);
    memset(n, 0, sizeof(*n));
}

static int query_one(VkPhysicalDevice phy, VkFormat vk_fmt, uint32_t drm_fmt,
                     struct wp_vk_mod_list *out, uint32_t *skipped_disjoint)
{
    VkDrmFormatModifierPropertiesListEXT mod_list = {
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
    };
    VkFormatProperties2 fmt_props = {
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
        .pNext = &mod_list,
    };
    VkDrmFormatModifierPropertiesEXT *mod_props;
    uint64_t *good;
    uint32_t good_n = 0, m;

    vkGetPhysicalDeviceFormatProperties2(phy, vk_fmt, &fmt_props);
    if (mod_list.drmFormatModifierCount == 0)
        return 0;
    mod_props = calloc(mod_list.drmFormatModifierCount, sizeof(*mod_props));
    if (!mod_props)
        return -ENOMEM;
    mod_list.pDrmFormatModifierProperties = mod_props;
    vkGetPhysicalDeviceFormatProperties2(phy, vk_fmt, &fmt_props);

    good = calloc(mod_list.drmFormatModifierCount, sizeof(uint64_t));
    if (!good) {
        free(mod_props);
        return -ENOMEM;
    }
    for (m = 0; m < mod_list.drmFormatModifierCount; m++) {
        uint64_t modifier = mod_props[m].drmFormatModifier;
        VkPhysicalDeviceImageDrmFormatModifierInfoEXT mod_info = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
            .drmFormatModifier = modifier,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        VkPhysicalDeviceExternalImageFormatInfo ext_info = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
            .pNext = &mod_info,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        };
        VkPhysicalDeviceImageFormatInfo2 img_info = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
            .pNext = &ext_info,
            .format = vk_fmt,
            .type = VK_IMAGE_TYPE_2D,
            .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        };
        VkExternalImageFormatProperties ext_props = {
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
        };
        VkImageFormatProperties2 img_props = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
            .pNext = &ext_props,
        };

        if (modifier == DRM_FORMAT_MOD_INVALID)
            continue;
        if (mod_props[m].drmFormatModifierPlaneCount > 1) {
            (*skipped_disjoint)++;
            continue;
        }
        if (vkGetPhysicalDeviceImageFormatProperties2(phy, &img_info, &img_props) != VK_SUCCESS)
            continue;
        if (!(ext_props.externalMemoryProperties.externalMemoryFeatures &
              VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT))
            continue;
        good[good_n++] = modifier;
    }
    free(mod_props);
    if (good_n == 0) {
        free(good);
        return 0;
    }
    out->vk_format = vk_fmt;
    out->drm_format = drm_fmt;
    out->modifiers = good;
    out->modifier_count = good_n;
    return 1;
}

int wp_vk_query_formats(VkPhysicalDevice phy, struct wp_vk_formats *out)
{
    size_t i;
    int ret;

    if (!phy || !out)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    out->v = calloc(sizeof(preferred) / sizeof(preferred[0]), sizeof(*out->v));
    if (!out->v)
        return -ENOMEM;
    for (i = 0; i < sizeof(preferred) / sizeof(preferred[0]); i++) {
        struct wp_vk_mod_list tmp = { 0 };
        ret = query_one(phy, preferred[i].vk_format, preferred[i].drm_format,
                        &tmp, &out->skipped_disjoint);
        if (ret < 0) {
            wp_vk_formats_free(out);
            return ret;
        }
        if (ret > 0)
            out->v[out->n++] = tmp;
    }
    if (out->n == 0) {
        wp_vk_formats_free(out);
        return -ENODEV;
    }
    return 0;
}

static bool device_ok(dev_t target, dev_t main_dev, dev_t render, dev_t primary)
{
    if (target == (dev_t)0)
        return true;
    if (target == main_dev || target == render || target == primary)
        return true;
    return false;
}

int wp_negotiate(const struct wp_feedback *fb, const struct wp_vk_formats *vk,
                 dev_t render, dev_t primary, struct wp_negotiated *out)
{
    int64_t best_score = -1;
    uint32_t t, p;

    if (!fb || !fb->done || fb->ntranches == 0 || !vk || vk->n == 0 || !out)
        return -EINVAL;
    memset(out, 0, sizeof(*out));

    for (t = 0; t < fb->ntranches; t++) {
        const struct wp_tranche *tr = &fb->tranches[t];
        bool scanout = (tr->flags & WP_TRANCHE_SCANOUT) != 0;

        if (!device_ok(tr->target_device, fb->main_device, render, primary))
            continue;
        if (scanout && tr->target_device != (dev_t)0 &&
            tr->target_device != render && tr->target_device != primary &&
            tr->target_device != fb->main_device)
            continue;

        for (p = 0; p < tr->pair_count; p++) {
            uint32_t drm_fmt = tr->pairs[p].format;
            uint64_t mod = tr->pairs[p].modifier;
            const struct wp_vk_mod_list *ent = NULL;
            uint32_t v, m, tp;
            bool mod_ok = false;
            int64_t score = 0;
            uint64_t *inter;
            uint32_t ninter = 0;
            size_t fi;

            if (mod == DRM_FORMAT_MOD_INVALID)
                continue;
            for (v = 0; v < vk->n; v++) {
                if (vk->v[v].drm_format == drm_fmt) {
                    ent = &vk->v[v];
                    break;
                }
            }
            if (!ent)
                continue;
            for (m = 0; m < ent->modifier_count; m++) {
                if (ent->modifiers[m] == mod) {
                    mod_ok = true;
                    break;
                }
            }
            if (!mod_ok)
                continue;
            if (scanout)
                score += 1000000;
            for (fi = 0; fi < sizeof(preferred) / sizeof(preferred[0]); fi++) {
                if (preferred[fi].drm_format == drm_fmt) {
                    score += (int64_t)(1000 - (int)fi * 10);
                    break;
                }
            }
            if (mod != DRM_FORMAT_MOD_LINEAR)
                score += 100;
            if (score <= best_score)
                continue;

            inter = malloc(tr->pair_count * sizeof(uint64_t));
            if (!inter) {
                wp_negotiated_free(out);
                return -ENOMEM;
            }
            for (tp = 0; tp < tr->pair_count; tp++) {
                uint32_t vm;
                if (tr->pairs[tp].format != drm_fmt)
                    continue;
                for (vm = 0; vm < ent->modifier_count; vm++) {
                    if (ent->modifiers[vm] == tr->pairs[tp].modifier) {
                        inter[ninter++] = tr->pairs[tp].modifier;
                        break;
                    }
                }
            }
            if (ninter == 0) {
                free(inter);
                continue;
            }
            free(out->modifiers);
            out->modifiers = inter;
            out->modifier_count = ninter;
            out->drm_format = drm_fmt;
            out->vk_format = ent->vk_format;
            out->scanout = scanout;
            out->target_device = tr->target_device;
            out->valid = true;
            best_score = score;
        }
    }
    if (!out->valid)
        return -ENODEV;
    return 0;
}

void wp_vk_formats_print(const struct wp_vk_formats *f, FILE *fp)
{
    uint32_t i, m;
    if (!f || !fp)
        return;
    fprintf(fp, "vulkan exportable formats %u  skipped_disjoint %u\n",
            f->n, f->skipped_disjoint);
    for (i = 0; i < f->n; i++) {
        fprintf(fp, "  %s drm 0x%08x vk %u  %u modifiers\n",
                fmt_name(f->v[i].drm_format), f->v[i].drm_format,
                (unsigned)f->v[i].vk_format, f->v[i].modifier_count);
        for (m = 0; m < f->v[i].modifier_count && m < 8; m++)
            fprintf(fp, "    0x%016" PRIx64 "\n", f->v[i].modifiers[m]);
        if (f->v[i].modifier_count > 8)
            fprintf(fp, "    ... (%u more)\n", f->v[i].modifier_count - 8);
    }
}

void wp_negotiated_print(const struct wp_negotiated *n, FILE *fp)
{
    uint32_t i;
    if (!n || !fp)
        return;
    if (!n->valid) {
        fprintf(fp, "negotiated: invalid\n");
        return;
    }
    fprintf(fp, "negotiated %s (0x%08x) vk %u scanout %s target %lu  %u modifiers\n",
            fmt_name(n->drm_format), n->drm_format, (unsigned)n->vk_format,
            n->scanout ? "yes" : "no", (unsigned long)n->target_device,
            n->modifier_count);
    for (i = 0; i < n->modifier_count; i++)
        fprintf(fp, "  [%u] 0x%016" PRIx64 "%s\n", i, n->modifiers[i],
                n->chosen_modifier == n->modifiers[i] ? " chosen" : "");
}

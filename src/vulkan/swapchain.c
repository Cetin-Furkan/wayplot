#define _GNU_SOURCE
#include "vulkan/swapchain.h"

#include "helper/drmfd.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void wp_swapchain_init(struct wp_swapchain *sc)
{
    uint32_t i;
    if (!sc)
        return;
    memset(sc, 0, sizeof(*sc));
    for (i = 0; i < WP_SWAPCHAIN_IMAGES; i++) {
        sc->images[i].dma_fd = -1;
        sc->images[i].release_fd = -1;
    }
}

static uint32_t plane_count_for(VkPhysicalDevice phy, VkFormat format, uint64_t modifier)
{
    VkDrmFormatModifierPropertiesListEXT mod_list = {
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
    };
    VkFormatProperties2 fmt_props = {
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
        .pNext = &mod_list,
    };
    VkDrmFormatModifierPropertiesEXT *props;
    uint32_t n = 1, i;

    vkGetPhysicalDeviceFormatProperties2(phy, format, &fmt_props);
    if (mod_list.drmFormatModifierCount == 0)
        return 1;
    props = calloc(mod_list.drmFormatModifierCount, sizeof(*props));
    if (!props)
        return 1;
    mod_list.pDrmFormatModifierProperties = props;
    vkGetPhysicalDeviceFormatProperties2(phy, format, &fmt_props);
    for (i = 0; i < mod_list.drmFormatModifierCount; i++) {
        if (props[i].drmFormatModifier == modifier) {
            n = props[i].drmFormatModifierPlaneCount;
            break;
        }
    }
    free(props);
    if (n == 0)
        n = 1;
    if (n > 4)
        n = 4;
    return n;
}

void wp_swapchain_destroy_image(struct wp_device *d, struct wp_vk_image *img)
{
    if (!d || !img)
        return;
    if (img->cmd)
        vkFreeCommandBuffers(d->device, d->pool, 1, &img->cmd);
    if (img->view)
        vkDestroyImageView(d->device, img->view, NULL);
    if (img->image)
        vkDestroyImage(d->device, img->image, NULL);
    if (img->memory)
        vkFreeMemory(d->device, img->memory, NULL);
    if (img->dma_fd >= 0)
        close(img->dma_fd);
    if (img->drm_handle)
        wp_drm_syncobj_destroy(d->drm_fd, img->drm_handle);
    wp_timeline_destroy(d, &img->release_sem, &img->release_fd);
    memset(img, 0, sizeof(*img));
    img->dma_fd = -1;
    img->release_fd = -1;
}

static int create_one(struct wp_device *d, struct wp_vk_image *img,
                      uint32_t width, uint32_t height, const struct wp_negotiated *np,
                      VkPhysicalDeviceMemoryProperties *mprops)
{
    VkImageDrmFormatModifierListCreateInfoEXT mod_list = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
        .drmFormatModifierCount = np->modifier_count,
        .pDrmFormatModifiers = np->modifiers,
    };
    VkExternalMemoryImageCreateInfo ext_img = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &mod_list,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &ext_img,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = np->vk_format,
        .extent = { width, height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkDeviceImageMemoryRequirements q = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS,
        .pCreateInfo = &ici,
    };
    VkMemoryDedicatedRequirements ded_req = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
    };
    VkMemoryRequirements2 memreq = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
        .pNext = &ded_req,
    };
    VkExportMemoryAllocateInfo exp = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkMemoryDedicatedAllocateInfo ded = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = &exp,
    };
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkBindImageMemoryInfo bind = { .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO };
    VkImageDrmFormatModifierPropertiesEXT modp = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT,
    };
    VkMemoryGetFdInfoKHR getfd = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = np->vk_format,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    uint32_t idx, p;
    int ret;

    memset(img, 0, sizeof(*img));
    img->dma_fd = -1;
    img->release_fd = -1;

    vkGetDeviceImageMemoryRequirements(d->device, &q, &memreq);

    if (vkCreateImage(d->device, &ici, NULL, &img->image) != VK_SUCCESS)
        return -EIO;
    if (d->vkGetImageDrmFormatModifierPropertiesEXT(d->device, img->image, &modp) != VK_SUCCESS) {
        wp_swapchain_destroy_image(d, img);
        return -EIO;
    }
    img->modifier = modp.drmFormatModifier;
    img->plane_count = plane_count_for(d->phy, np->vk_format, img->modifier);
    if (img->plane_count != 1) {
        fprintf(stderr, "driver selected disjoint modifier with %u planes\n", img->plane_count);
        wp_swapchain_destroy_image(d, img);
        return -EPROTO;
    }

    idx = wp_find_memory_type(mprops, memreq.memoryRequirements.memoryTypeBits,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (idx == UINT32_MAX)
        idx = wp_find_memory_type(mprops, memreq.memoryRequirements.memoryTypeBits, 0);
    if (idx == UINT32_MAX) {
        wp_swapchain_destroy_image(d, img);
        return -ENOMEM;
    }
    ded.image = img->image;
    ai.pNext = &ded;
    ai.allocationSize = memreq.memoryRequirements.size;
    ai.memoryTypeIndex = idx;
    if (vkAllocateMemory(d->device, &ai, NULL, &img->memory) != VK_SUCCESS) {
        idx = wp_find_memory_type(mprops, memreq.memoryRequirements.memoryTypeBits, 0);
        if (idx == UINT32_MAX) {
            wp_swapchain_destroy_image(d, img);
            return -ENOMEM;
        }
        ai.memoryTypeIndex = idx;
        if (vkAllocateMemory(d->device, &ai, NULL, &img->memory) != VK_SUCCESS) {
            wp_swapchain_destroy_image(d, img);
            return -ENOMEM;
        }
    }
    bind.image = img->image;
    bind.memory = img->memory;
    if (vkBindImageMemory2(d->device, 1, &bind) != VK_SUCCESS) {
        wp_swapchain_destroy_image(d, img);
        return -EIO;
    }
    for (p = 0; p < img->plane_count; p++) {
        VkImageSubresource2 sub = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_SUBRESOURCE_2,
            .imageSubresource = {
                .aspectMask = (VkImageAspectFlags)(VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT << p),
            },
        };
        VkSubresourceLayout2 lay = { .sType = VK_STRUCTURE_TYPE_SUBRESOURCE_LAYOUT_2 };
        vkGetImageSubresourceLayout2(d->device, img->image, &sub, &lay);
        img->planes[p].stride = (uint32_t)lay.subresourceLayout.rowPitch;
        img->planes[p].offset = (uint32_t)lay.subresourceLayout.offset;
    }
    getfd.memory = img->memory;
    if (d->vkGetMemoryFdKHR(d->device, &getfd, &img->dma_fd) != VK_SUCCESS || img->dma_fd < 0) {
        wp_swapchain_destroy_image(d, img);
        return -EIO;
    }
    vci.image = img->image;
    if (vkCreateImageView(d->device, &vci, NULL, &img->view) != VK_SUCCESS) {
        wp_swapchain_destroy_image(d, img);
        return -EIO;
    }
    ret = wp_timeline_create(d, &img->release_sem, &img->release_fd);
    if (ret < 0) {
        wp_swapchain_destroy_image(d, img);
        return ret;
    }
    ret = wp_drm_syncobj_fd_to_handle(d->drm_fd, img->release_fd, &img->drm_handle);
    if (ret < 0) {
        wp_swapchain_destroy_image(d, img);
        return ret;
    }
    return 0;
}

int wp_swapchain_create(struct wp_device *d, struct wp_swapchain *sc,
                        uint32_t width, uint32_t height, const struct wp_negotiated *np)
{
    VkPhysicalDeviceMemoryProperties mprops;
    VkCommandBuffer cmds[WP_SWAPCHAIN_IMAGES];
    VkCommandBufferAllocateInfo cai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = WP_SWAPCHAIN_IMAGES,
    };
    uint32_t i;
    int ret;

    if (!d || !sc || !np || !np->valid || np->modifier_count == 0)
        return -EINVAL;
    if (width == 0 || height == 0)
        return -EINVAL;
    if (sc->allocated)
        return -EBUSY;

    vkGetPhysicalDeviceMemoryProperties(d->phy, &mprops);
    wp_negotiated_free(&sc->params);
    sc->params = (struct wp_negotiated){
        .drm_format = np->drm_format,
        .vk_format = np->vk_format,
        .modifier_count = np->modifier_count,
        .scanout = np->scanout,
        .target_device = np->target_device,
        .valid = true,
    };
    sc->params.modifiers = malloc(np->modifier_count * sizeof(uint64_t));
    if (!sc->params.modifiers)
        return -ENOMEM;
    memcpy(sc->params.modifiers, np->modifiers, np->modifier_count * sizeof(uint64_t));

    for (i = 0; i < WP_SWAPCHAIN_IMAGES; i++) {
        ret = create_one(d, &sc->images[i], width, height, &sc->params, &mprops);
        if (ret < 0) {
            while (i > 0) {
                i--;
                wp_swapchain_destroy_image(d, &sc->images[i]);
            }
            wp_negotiated_free(&sc->params);
            return ret;
        }
        sc->params.chosen_modifier = sc->images[i].modifier;
    }
    cai.commandPool = d->pool;
    if (vkAllocateCommandBuffers(d->device, &cai, cmds) != VK_SUCCESS) {
        for (i = 0; i < WP_SWAPCHAIN_IMAGES; i++)
            wp_swapchain_destroy_image(d, &sc->images[i]);
        wp_negotiated_free(&sc->params);
        return -EIO;
    }
    for (i = 0; i < WP_SWAPCHAIN_IMAGES; i++)
        sc->images[i].cmd = cmds[i];
    sc->width = width;
    sc->height = height;
    sc->allocated = true;
    sc->cursor = 0;
    return 0;
}

void wp_swapchain_retire(struct wp_swapchain *sc)
{
    uint32_t need, cap;
    struct wp_vk_image *grown;
    uint32_t i;

    if (!sc || !sc->allocated)
        return;
    need = sc->retired_count + WP_SWAPCHAIN_IMAGES;
    cap = sc->retired_cap ? sc->retired_cap : WP_SWAPCHAIN_IMAGES * 4;
    while (cap < need)
        cap *= 2;
    grown = realloc(sc->retired, cap * sizeof(*grown));
    if (!grown)
        return;
    sc->retired = grown;
    sc->retired_cap = cap;
    memcpy(sc->retired + sc->retired_count, sc->images, sizeof(sc->images));
    sc->retired_count += WP_SWAPCHAIN_IMAGES;
    memset(sc->images, 0, sizeof(sc->images));
    for (i = 0; i < WP_SWAPCHAIN_IMAGES; i++) {
        sc->images[i].dma_fd = -1;
        sc->images[i].release_fd = -1;
    }
    sc->allocated = false;
    sc->cursor = 0;
}

void wp_swapchain_free_retired(struct wp_device *d, struct wp_swapchain *sc)
{
    uint32_t i, kept = 0;
    uint64_t counter = 0;

    if (!d || !sc)
        return;
    for (i = 0; i < sc->retired_count; i++) {
        struct wp_vk_image *img = &sc->retired[i];
        if (img->release_sem && img->last_release) {
            if (vkGetSemaphoreCounterValue(d->device, img->release_sem, &counter) != VK_SUCCESS)
                counter = 0;
            if (counter < img->last_release) {
                sc->retired[kept++] = *img;
                continue;
            }
        }
        wp_swapchain_destroy_image(d, img);
    }
    sc->retired_count = kept;
}

int wp_swapchain_wait_gpu(struct wp_device *d, const struct wp_vk_image *img)
{
    VkSemaphoreWaitInfo wi = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
    };
    uint64_t value;

    if (!d || !img)
        return -EINVAL;
    if (img->last_acquire == 0)
        return 0;
    value = img->last_acquire;
    wi.pSemaphores = &d->acquire_sem;
    wi.pValues = &value;
    if (vkWaitSemaphores(d->device, &wi, UINT64_MAX) != VK_SUCCESS)
        return -EIO;
    return 0;
}

int wp_swapchain_pick(struct wp_device *d, struct wp_swapchain *sc, uint32_t *out)
{
    uint32_t i;

    if (!d || !sc || !out || !sc->allocated)
        return -EINVAL;
    for (i = 0; i < WP_SWAPCHAIN_IMAGES; i++) {
        uint32_t idx = (sc->cursor + i) % WP_SWAPCHAIN_IMAGES;
        struct wp_vk_image *img = &sc->images[idx];
        uint64_t counter = 0;

        if (img->last_release == 0) {
            *out = idx;
            return 0;
        }
        if (vkGetSemaphoreCounterValue(d->device, img->release_sem, &counter) != VK_SUCCESS)
            return -EIO;
        if (counter >= img->last_release) {
            *out = idx;
            return 0;
        }
    }
    return -EAGAIN;
}

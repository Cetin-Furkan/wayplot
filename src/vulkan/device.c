#define _GNU_SOURCE
#include "vulkan/device.h"

#include "helper/drmfd.h"
#include "helper/log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int vkerr(VkResult r)
{
    return r == VK_SUCCESS ? 0 : -EIO;
}

uint32_t wp_find_memory_type(VkPhysicalDeviceMemoryProperties *props,
                             uint32_t type_bits, VkMemoryPropertyFlags prefer)
{
    uint32_t i;

    if (prefer) {
        for (i = 0; i < props->memoryTypeCount; i++) {
            if ((type_bits & (1u << i)) &&
                (props->memoryTypes[i].propertyFlags & prefer) == prefer)
                return i;
        }
    }
    for (i = 0; i < props->memoryTypeCount; i++) {
        if (type_bits & (1u << i))
            return i;
    }
    return UINT32_MAX;
}

int wp_timeline_create(struct wp_device *d, VkSemaphore *sem, int *fd)
{
    VkExportSemaphoreCreateInfo exp = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    VkSemaphoreTypeCreateInfo type = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .pNext = &exp,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = 0,
    };
    VkSemaphoreCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &type,
    };
    VkSemaphoreGetFdInfoKHR getfd = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
    };

    if (!d || !sem || !fd)
        return -EINVAL;
    *sem = VK_NULL_HANDLE;
    *fd = -1;
    if (vkCreateSemaphore(d->device, &ci, NULL, sem) != VK_SUCCESS)
        return -EIO;
    getfd.semaphore = *sem;
    if (d->vkGetSemaphoreFdKHR(d->device, &getfd, fd) != VK_SUCCESS || *fd < 0) {
        vkDestroySemaphore(d->device, *sem, NULL);
        *sem = VK_NULL_HANDLE;
        return -EIO;
    }
    return 0;
}

void wp_timeline_destroy(struct wp_device *d, VkSemaphore *sem, int *fd)
{
    if (fd && *fd >= 0) {
        close(*fd);
        *fd = -1;
    }
    if (d && sem && *sem != VK_NULL_HANDLE) {
        vkDestroySemaphore(d->device, *sem, NULL);
        *sem = VK_NULL_HANDLE;
    }
}

static int make_push_layout(struct wp_device *d)
{
    VkDescriptorSetLayoutBinding binds[2] = {
        {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        },
        {
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
    };
    VkDescriptorSetLayoutCreateInfo set_ci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT,
        .bindingCount = 2,
        .pBindings = binds,
    };
    VkPipelineLayoutCreateInfo lay_ci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
    };

    if (vkCreateDescriptorSetLayout(d->device, &set_ci, NULL, &d->push_set) != VK_SUCCESS)
        return -EIO;
    lay_ci.pSetLayouts = &d->push_set;
    if (vkCreatePipelineLayout(d->device, &lay_ci, NULL, &d->push_layout) != VK_SUCCESS)
        return -EIO;
    return 0;
}

static bool exportable_timeline(VkPhysicalDevice phy)
{
    VkSemaphoreTypeCreateInfo type = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
    };
    VkPhysicalDeviceExternalSemaphoreInfo info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
        .pNext = &type,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    VkExternalSemaphoreProperties props = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES,
    };

    vkGetPhysicalDeviceExternalSemaphoreProperties(phy, &info, &props);
    return (props.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT) != 0;
}

int wp_device_prove_host_copy(struct wp_device *d)
{
    const uint32_t w = 4, h = 4;
    uint32_t src[16], dst[16];
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties mprops;
    VkMemoryRequirements2 memreq = { .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
    VkDeviceImageMemoryRequirements q = { .sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS };
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { w, h, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_LINEAR,
        .usage = VK_IMAGE_USAGE_HOST_TRANSFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
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
        .pHostPointer = src,
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .layerCount = 1,
        },
        .imageExtent = { w, h, 1 },
    };
    VkCopyMemoryToImageInfo cpy = {
        .sType = VK_STRUCTURE_TYPE_COPY_MEMORY_TO_IMAGE_INFO,
        .dstImageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .regionCount = 1,
        .pRegions = &region,
    };
    VkImageToMemoryCopy back_region = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_TO_MEMORY_COPY,
        .pHostPointer = dst,
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .layerCount = 1,
        },
        .imageExtent = { w, h, 1 },
    };
    VkCopyImageToMemoryInfo back = {
        .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_MEMORY_INFO,
        .srcImageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .regionCount = 1,
        .pRegions = &back_region,
    };
    uint32_t i, idx;
    VkResult vr;

    if (!d || !d->host_image_copy)
        return -ENOTSUP;
    for (i = 0; i < 16; i++)
        src[i] = 0xff00ff00u ^ (i * 0x01010101u);
    memset(dst, 0, sizeof(dst));

    q.pCreateInfo = &ici;
    vkGetDeviceImageMemoryRequirements(d->device, &q, &memreq);
    if (vkCreateImage(d->device, &ici, NULL, &img) != VK_SUCCESS)
        return -EIO;
    vkGetPhysicalDeviceMemoryProperties(d->phy, &mprops);
    idx = wp_find_memory_type(&mprops, memreq.memoryRequirements.memoryTypeBits,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (idx == UINT32_MAX)
        idx = wp_find_memory_type(&mprops, memreq.memoryRequirements.memoryTypeBits, 0);
    if (idx == UINT32_MAX) {
        vkDestroyImage(d->device, img, NULL);
        return -ENOMEM;
    }
    ai.allocationSize = memreq.memoryRequirements.size;
    ai.memoryTypeIndex = idx;
    if (vkAllocateMemory(d->device, &ai, NULL, &mem) != VK_SUCCESS) {
        vkDestroyImage(d->device, img, NULL);
        return -ENOMEM;
    }
    if (vkBindImageMemory(d->device, img, mem, 0) != VK_SUCCESS) {
        vkFreeMemory(d->device, mem, NULL);
        vkDestroyImage(d->device, img, NULL);
        return -EIO;
    }
    tr.image = img;
    vr = vkTransitionImageLayout(d->device, 1, &tr);
    if (vr != VK_SUCCESS)
        goto fail;
    cpy.dstImage = img;
    vr = vkCopyMemoryToImage(d->device, &cpy);
    if (vr != VK_SUCCESS)
        goto fail;
    back.srcImage = img;
    vr = vkCopyImageToMemory(d->device, &back);
    if (vr != VK_SUCCESS)
        goto fail;
    if (memcmp(src, dst, sizeof(src)) != 0) {
        vr = VK_ERROR_UNKNOWN;
        goto fail;
    }
    vkDestroyImage(d->device, img, NULL);
    vkFreeMemory(d->device, mem, NULL);
    return 0;
fail:
    vkDestroyImage(d->device, img, NULL);
    vkFreeMemory(d->device, mem, NULL);
    return vkerr(vr);
}

static int pick_queues(VkPhysicalDevice phy, uint32_t *gfx, uint32_t *xfer)
{
    uint32_t n = 0, i;
    VkQueueFamilyProperties *q;

    vkGetPhysicalDeviceQueueFamilyProperties(phy, &n, NULL);
    if (n == 0)
        return -ENODEV;
    q = calloc(n, sizeof(*q));
    if (!q)
        return -ENOMEM;
    vkGetPhysicalDeviceQueueFamilyProperties(phy, &n, q);
    *gfx = UINT32_MAX;
    *xfer = UINT32_MAX;
    for (i = 0; i < n; i++) {
        if ((q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && *gfx == UINT32_MAX)
            *gfx = i;
        if ((q[i].queueFlags & VK_QUEUE_TRANSFER_BIT) &&
            !(q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && *xfer == UINT32_MAX)
            *xfer = i;
    }
    free(q);
    if (*gfx == UINT32_MAX)
        return -ENODEV;
    if (*xfer == UINT32_MAX)
        *xfer = *gfx;
    return 0;
}

int wp_device_open(struct wp_device *d, dev_t compositor_dev)
{
    struct wp_gpu_caps caps;
    const struct wp_gpu_info *info;
    VkPhysicalDeviceVulkan14Features f14 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
    };
    VkPhysicalDeviceVulkan13Features f13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .pNext = &f14,
        .dynamicRendering = VK_TRUE,
        .synchronization2 = VK_TRUE,
    };
    VkPhysicalDeviceVulkan12Features f12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &f13,
        .timelineSemaphore = VK_TRUE,
    };
    VkPhysicalDeviceVulkan14Features have14 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
    };
    VkPhysicalDeviceVulkan13Features have13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .pNext = &have14,
    };
    VkPhysicalDeviceVulkan12Features have12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &have13,
    };
    VkPhysicalDeviceFeatures2 have = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &have12,
    };
    VkPhysicalDeviceVulkan14Properties p14 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_PROPERTIES,
    };
    VkPhysicalDeviceProperties2 props2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &p14,
    };
    const char *exts[] = {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    };
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci[2];
    uint32_t nq;
    VkDeviceCreateInfo dci;
    VkCommandPoolCreateInfo pool_ci;
    int ret;

    if (!d)
        return -EINVAL;
    memset(d, 0, sizeof(*d));
    d->acquire_fd = -1;
    d->drm_fd = -1;

    ret = wp_gpu_enumerate(&caps);
    if (ret < 0)
        return ret;
    ret = wp_gpu_pick(&caps, compositor_dev);
    if (ret < 0) {
        fprintf(stderr, "no Vulkan 1.4 GPU matches compositor DRM node %lu\n",
                (unsigned long)compositor_dev);
        wp_gpu_caps_print(&caps, stderr);
        wp_gpu_caps_free(&caps);
        return ret;
    }
    info = &caps.v[caps.picked_index];
    d->instance = caps.instance;
    d->phy = caps.picked;
    d->render = info->render;
    d->primary = info->primary;
    caps.instance = VK_NULL_HANDLE;
    wp_gpu_caps_free(&caps);

    vkGetPhysicalDeviceFeatures2(d->phy, &have);
    vkGetPhysicalDeviceProperties2(d->phy, &props2);
    d->max_push_descriptors = p14.maxPushDescriptors;
    if (!have12.timelineSemaphore || !have13.dynamicRendering || !have13.synchronization2) {
        fprintf(stderr, "device missing timeline/dynamicRendering/sync2\n");
        wp_device_close(d);
        return -ENODEV;
    }
    if (!have14.pushDescriptor || p14.maxPushDescriptors < 2) {
        fprintf(stderr, "device missing pushDescriptor (max %u)\n", p14.maxPushDescriptors);
        wp_device_close(d);
        return -ENODEV;
    }
    if (!have14.maintenance5) {
        fprintf(stderr, "device missing maintenance5\n");
        wp_device_close(d);
        return -ENODEV;
    }
    if (!exportable_timeline(d->phy)) {
        fprintf(stderr, "timeline semaphore is not exportable as opaque fd\n");
        wp_device_close(d);
        return -ENODEV;
    }

    f14.pushDescriptor = VK_TRUE;
    f14.maintenance5 = VK_TRUE;
    f14.hostImageCopy = have14.hostImageCopy ? VK_TRUE : VK_FALSE;
    d->host_image_copy = have14.hostImageCopy;
    d->push_descriptor = true;
    d->maintenance5 = true;

    ret = pick_queues(d->phy, &d->gfx_family, &d->xfer_family);
    if (ret < 0) {
        wp_device_close(d);
        return ret;
    }
    nq = 1;
    qci[0] = (VkDeviceQueueCreateInfo){
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = d->gfx_family,
        .queueCount = 1,
        .pQueuePriorities = &prio,
    };
    if (d->xfer_family != d->gfx_family) {
        qci[1] = qci[0];
        qci[1].queueFamilyIndex = d->xfer_family;
        nq = 2;
    }

    memset(&dci, 0, sizeof(dci));
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &f12;
    dci.queueCreateInfoCount = nq;
    dci.pQueueCreateInfos = qci;
    dci.enabledExtensionCount = 5;
    dci.ppEnabledExtensionNames = exts;
    if (vkCreateDevice(d->phy, &dci, NULL, &d->device) != VK_SUCCESS) {
        fprintf(stderr, "vkCreateDevice failed\n");
        wp_device_close(d);
        return -EIO;
    }
    vkGetDeviceQueue(d->device, d->gfx_family, 0, &d->gfx);
    vkGetDeviceQueue(d->device, d->xfer_family, 0, &d->xfer);

    d->vkGetMemoryFdKHR = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(d->device, "vkGetMemoryFdKHR");
    d->vkGetSemaphoreFdKHR = (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(d->device, "vkGetSemaphoreFdKHR");
    d->vkGetImageDrmFormatModifierPropertiesEXT =
        (PFN_vkGetImageDrmFormatModifierPropertiesEXT)vkGetDeviceProcAddr(
            d->device, "vkGetImageDrmFormatModifierPropertiesEXT");
    if (!d->vkGetMemoryFdKHR || !d->vkGetSemaphoreFdKHR || !d->vkGetImageDrmFormatModifierPropertiesEXT) {
        fprintf(stderr, "missing dma-buf/syncobj entry points\n");
        wp_device_close(d);
        return -EIO;
    }

    pool_ci = (VkCommandPoolCreateInfo){
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = d->gfx_family,
    };
    if (vkCreateCommandPool(d->device, &pool_ci, NULL, &d->pool) != VK_SUCCESS) {
        wp_device_close(d);
        return -EIO;
    }
    if (make_push_layout(d) < 0) {
        fprintf(stderr, "push descriptor set layout failed\n");
        wp_device_close(d);
        return -EIO;
    }
    if (wp_timeline_create(d, &d->acquire_sem, &d->acquire_fd) < 0) {
        fprintf(stderr, "acquire timeline export failed\n");
        wp_device_close(d);
        return -EIO;
    }
    d->drm_fd = wp_drm_open_render(d->render);
    if (d->drm_fd < 0) {
        fprintf(stderr, "open render node for DRM syncobj eventfd failed\n");
        wp_device_close(d);
        return d->drm_fd;
    }
    if (d->host_image_copy) {
        ret = wp_device_prove_host_copy(d);
        if (ret < 0) {
            fprintf(stderr, "host image copy prove failed (%s)\n", strerror(-ret));
            wp_device_close(d);
            return ret;
        }
    }
    wp_debug("device open hostCopy=%d pushDesc=%u drm_fd=%d\n",
             d->host_image_copy, d->max_push_descriptors, d->drm_fd);
    return 0;
}

void wp_device_close(struct wp_device *d)
{
    if (!d)
        return;
    wp_timeline_destroy(d, &d->acquire_sem, &d->acquire_fd);
    if (d->push_layout)
        vkDestroyPipelineLayout(d->device, d->push_layout, NULL);
    if (d->push_set)
        vkDestroyDescriptorSetLayout(d->device, d->push_set, NULL);
    if (d->pool)
        vkDestroyCommandPool(d->device, d->pool, NULL);
    if (d->device)
        vkDestroyDevice(d->device, NULL);
    if (d->instance)
        vkDestroyInstance(d->instance, NULL);
    if (d->drm_fd >= 0)
        close(d->drm_fd);
    memset(d, 0, sizeof(*d));
    d->acquire_fd = -1;
    d->drm_fd = -1;
}

void wp_device_print(const struct wp_device *d, FILE *out)
{
    if (!d || !out || !d->device)
        return;
    fprintf(out, "vk device  hostImageCopy %d  pushDescriptor %d (max %u)  maintenance5 %d\n",
            d->host_image_copy, d->push_descriptor, d->max_push_descriptors, d->maintenance5);
    fprintf(out, "  gfx family %u  xfer family %u  acquire_fd %d  drm_fd %d  render %lu\n",
            d->gfx_family, d->xfer_family, d->acquire_fd, d->drm_fd, (unsigned long)d->render);
}

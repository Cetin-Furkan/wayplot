#define _GNU_SOURCE
#include "vulkan/gpu.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysmacros.h>

static bool has_ext(VkPhysicalDevice phy, const char *name)
{
    uint32_t n = 0, i;
    VkExtensionProperties *exts;
    bool found = false;

    vkEnumerateDeviceExtensionProperties(phy, NULL, &n, NULL);
    if (n == 0)
        return false;
    exts = calloc(n, sizeof(*exts));
    if (!exts)
        return false;
    vkEnumerateDeviceExtensionProperties(phy, NULL, &n, exts);
    for (i = 0; i < n; i++) {
        if (strcmp(exts[i].extensionName, name) == 0) {
            found = true;
            break;
        }
    }
    free(exts);
    return found;
}

static void fill_one(VkPhysicalDevice phy, struct wp_gpu_info *g)
{
    VkPhysicalDeviceProperties2 props2;
    VkPhysicalDeviceDrmPropertiesEXT drm;
    VkPhysicalDeviceFeatures2 feat2;
    VkPhysicalDeviceVulkan12Features f12;
    VkPhysicalDeviceVulkan13Features f13;
    VkPhysicalDeviceVulkan14Features f14;

    memset(g, 0, sizeof(*g));

    memset(&drm, 0, sizeof(drm));
    drm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
    memset(&props2, 0, sizeof(props2));
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    g->phy_drm = has_ext(phy, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME);
    if (g->phy_drm)
        props2.pNext = &drm;
    vkGetPhysicalDeviceProperties2(phy, &props2);

    snprintf(g->name, sizeof(g->name), "%s", props2.properties.deviceName);
    g->api_version = props2.properties.apiVersion;
    g->vendor_id = props2.properties.vendorID;
    g->device_id = props2.properties.deviceID;
    g->driver_version = props2.properties.driverVersion;
    g->api_1_4 = VK_API_VERSION_MAJOR(g->api_version) > 1 ||
                 (VK_API_VERSION_MAJOR(g->api_version) == 1 &&
                  VK_API_VERSION_MINOR(g->api_version) >= 4);

    if (g->phy_drm) {
        g->has_render = drm.hasRender;
        g->has_primary = drm.hasPrimary;
        if (drm.hasRender)
            g->render = makedev((unsigned)drm.renderMajor, (unsigned)drm.renderMinor);
        if (drm.hasPrimary)
            g->primary = makedev((unsigned)drm.primaryMajor, (unsigned)drm.primaryMinor);
    }

    g->dma_buf = has_ext(phy, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    g->drm_modifier = has_ext(phy, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
    g->memory_fd = has_ext(phy, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    g->semaphore_fd = has_ext(phy, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);

    memset(&f14, 0, sizeof(f14));
    f14.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES;
    memset(&f13, 0, sizeof(f13));
    f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    f13.pNext = &f14;
    memset(&f12, 0, sizeof(f12));
    f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    f12.pNext = &f13;
    memset(&feat2, 0, sizeof(feat2));
    feat2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    feat2.pNext = &f12;
    vkGetPhysicalDeviceFeatures2(phy, &feat2);
    g->push_descriptor = f14.pushDescriptor;
    g->host_image_copy = f14.hostImageCopy;
    g->maintenance5 = f14.maintenance5;
}

int wp_gpu_enumerate(struct wp_gpu_caps *c)
{
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "wayplot",
        .applicationVersion = 1,
        .apiVersion = VK_API_VERSION_1_4,
    };
    VkInstanceCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    uint32_t n = 0, i;
    VkPhysicalDevice *phy;
    VkResult vr;

    if (!c)
        return -EINVAL;
    memset(c, 0, sizeof(*c));
    c->picked_index = -1;

    vr = vkCreateInstance(&ci, NULL, &c->instance);
    if (vr != VK_SUCCESS)
        return -EPROTO;

    vr = vkEnumeratePhysicalDevices(c->instance, &n, NULL);
    if (vr != VK_SUCCESS || n == 0) {
        wp_gpu_caps_free(c);
        return -ENODEV;
    }
    phy = calloc(n, sizeof(*phy));
    c->v = calloc(n, sizeof(*c->v));
    if (!phy || !c->v) {
        free(phy);
        wp_gpu_caps_free(c);
        return -ENOMEM;
    }
    vr = vkEnumeratePhysicalDevices(c->instance, &n, phy);
    if (vr != VK_SUCCESS) {
        free(phy);
        wp_gpu_caps_free(c);
        return -EPROTO;
    }
    for (i = 0; i < n; i++) {
        fill_one(phy[i], &c->v[i]);
        c->v[i].phy = phy[i];
    }
    c->n = n;
    free(phy);
    return 0;
}

static bool matches(const struct wp_gpu_info *g, dev_t want)
{
    if (!g->api_1_4 || !g->dma_buf || !g->drm_modifier || !g->memory_fd || !g->semaphore_fd)
        return false;
    if (g->has_render && g->render == want)
        return true;
    if (g->has_primary && g->primary == want)
        return true;
    return false;
}

int wp_gpu_pick(struct wp_gpu_caps *c, dev_t compositor_dev)
{
    uint32_t i;

    if (!c || !c->instance || compositor_dev == (dev_t)0)
        return -EINVAL;
    c->picked_index = -1;
    c->picked = VK_NULL_HANDLE;
    for (i = 0; i < c->n; i++) {
        if (matches(&c->v[i], compositor_dev)) {
            c->picked_index = (int)i;
            c->picked = c->v[i].phy;
            return 0;
        }
    }
    return -ENODEV;
}

void wp_gpu_caps_print(const struct wp_gpu_caps *c, FILE *out)
{
    uint32_t i;

    if (!c || !out)
        return;
    fprintf(out, "%u vulkan physical device(s)\n", c->n);
    for (i = 0; i < c->n; i++) {
        const struct wp_gpu_info *g = &c->v[i];
        fprintf(out, "  [%u]%s %s  api %u.%u.%u vendor 0x%x device 0x%x\n",
                i, (int)i == c->picked_index ? " PICK" : "",
                g->name,
                VK_API_VERSION_MAJOR(g->api_version),
                VK_API_VERSION_MINOR(g->api_version),
                VK_API_VERSION_PATCH(g->api_version),
                g->vendor_id, g->device_id);
        fprintf(out, "      drm %s render %lu primary %lu  dma-buf %d modifier %d memfd %d syncfd %d\n",
                g->phy_drm ? "yes" : "no",
                (unsigned long)g->render, (unsigned long)g->primary,
                g->dma_buf, g->drm_modifier, g->memory_fd, g->semaphore_fd);
        fprintf(out, "      1.4 %d pushDescriptor %d hostImageCopy %d maintenance5 %d\n",
                g->api_1_4, g->push_descriptor, g->host_image_copy, g->maintenance5);
    }
}

void wp_gpu_caps_free(struct wp_gpu_caps *c)
{
    if (!c)
        return;
    free(c->v);
    if (c->instance)
        vkDestroyInstance(c->instance, NULL);
    memset(c, 0, sizeof(*c));
    c->picked_index = -1;
}

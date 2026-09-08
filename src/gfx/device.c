#include "khoros/gfx/device.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

static int khr_drm_open_render_node(dev_t render_dev) {
    if (render_dev == (dev_t)0) {
        return open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    }
    DIR* dir = opendir("/dev/dri");
    if (!dir) {
        return -1;
    }
    int found_fd = -1;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strncmp(entry->d_name, "renderD", 7) != 0) {
            continue;
        }
        char path[512];
        snprintf(path, sizeof(path), "/dev/dri/%s", entry->d_name);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        struct stat st;
        if (fstat(fd, &st) == 0 && st.st_rdev == render_dev) {
            found_fd = fd;
            break;
        }
        close(fd);
    }
    closedir(dir);
    if (found_fd < 0) {
        found_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    }
    return found_fd;
}

static void khr_check_device_extensions(VkPhysicalDevice phy, khr_device_candidate_t* c) {
    constexpr uint32_t MAX_EXTS = 256;
    VkExtensionProperties exts[MAX_EXTS];
    uint32_t count = MAX_EXTS;
    if (vkEnumerateDeviceExtensionProperties(phy, nullptr, &count, exts) != VK_SUCCESS &&
        count == 0) {
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        const char* name = exts[i].extensionName;
        if (strcmp(name, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) == 0) {
            c->has_ext_mem_fd = true;
        } else if (strcmp(name, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME) == 0) {
            c->has_ext_sem_fd = true;
        } else if (strcmp(name, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME) == 0) {
            c->has_drm_modifier = true;
        } else if (strcmp(name, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) == 0) {
            c->has_dma_buf = true;
        } else if (strcmp(name, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME) == 0) {
            c->has_drm_properties = true;
        } else if (strcmp(name, VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME) == 0) {
            c->has_host_copy_ext = true;
        }
    }
}

static bool khr_check_uma_memory(const VkPhysicalDeviceMemoryProperties* mem_props) {
    constexpr VkMemoryPropertyFlags required =
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < mem_props->memoryTypeCount; i++) {
        if ((mem_props->memoryTypes[i].propertyFlags & required) == required) {
            return true;
        }
    }
    return false;
}

static void khr_discover_queues(VkPhysicalDevice phy,
                                uint32_t* out_gfx,
                                uint32_t* out_compute,
                                uint32_t* out_xfer) {
    *out_gfx = UINT32_MAX;
    *out_compute = UINT32_MAX;
    *out_xfer = UINT32_MAX;

    VkQueueFamilyProperties props[KHR_MAX_QUEUE_FAMILIES];
    uint32_t count = KHR_MAX_QUEUE_FAMILIES;
    vkGetPhysicalDeviceQueueFamilyProperties(phy, &count, props);
    if (count == 0) {
        return;
    }

    // 1. Primary Graphics Queue (must support GRAPHICS)
    for (uint32_t i = 0; i < count; i++) {
        if (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            *out_gfx = i;
            break;
        }
    }

    // 2. Dedicated Compute Queue (COMPUTE but not GRAPHICS)
    for (uint32_t i = 0; i < count; i++) {
        if ((props[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
            !(props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            *out_compute = i;
            break;
        }
    }
    if (*out_compute == UINT32_MAX) {
        for (uint32_t i = 0; i < count; i++) {
            if (props[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                *out_compute = i;
                break;
            }
        }
    }

    // 3. Dedicated Transfer Queue (TRANSFER but neither GRAPHICS nor COMPUTE)
    for (uint32_t i = 0; i < count; i++) {
        if ((props[i].queueFlags & VK_QUEUE_TRANSFER_BIT) &&
            !(props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            !(props[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
            *out_xfer = i;
            break;
        }
    }
    if (*out_xfer == UINT32_MAX) {
        for (uint32_t i = 0; i < count; i++) {
            if ((props[i].queueFlags & VK_QUEUE_TRANSFER_BIT) &&
                !(props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                *out_xfer = i;
                break;
            }
        }
    }
    if (*out_xfer == UINT32_MAX) {
        for (uint32_t i = 0; i < count; i++) {
            if (props[i].queueFlags & VK_QUEUE_TRANSFER_BIT) {
                *out_xfer = i;
                break;
            }
        }
    }

    if (*out_xfer == UINT32_MAX) {
        *out_xfer = *out_gfx;
    }
    if (*out_compute == UINT32_MAX) {
        *out_compute = *out_gfx;
    }
}

static bool khr_query_device_candidate(VkPhysicalDevice phy, khr_device_candidate_t* c) {
    memset(c, 0, sizeof(*c));
    c->phy = phy;
    c->gfx_family = UINT32_MAX;
    c->compute_family = UINT32_MAX;
    c->xfer_family = UINT32_MAX;

    khr_check_device_extensions(phy, c);

    VkPhysicalDeviceDrmPropertiesEXT drm_props = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT,
    };
    VkPhysicalDeviceProperties2 props2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = c->has_drm_properties ? &drm_props : nullptr,
    };
    vkGetPhysicalDeviceProperties2(phy, &props2);
    c->props = props2.properties;

    uint32_t ver = c->props.apiVersion;
    c->is_vulkan_1_4 = (VK_API_VERSION_MAJOR(ver) > 1) ||
                       (VK_API_VERSION_MAJOR(ver) == 1 && VK_API_VERSION_MINOR(ver) >= 4);

    if (c->has_drm_properties) {
        if (drm_props.hasRender) {
            c->render_dev = makedev((unsigned)drm_props.renderMajor, (unsigned)drm_props.renderMinor);
        }
        if (drm_props.hasPrimary) {
            c->primary_dev = makedev((unsigned)drm_props.primaryMajor, (unsigned)drm_props.primaryMinor);
        }
    }

    c->fhost.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES;
    c->f6.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES;
    c->f6.pNext = &c->fhost;
    c->f5.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES;
    c->f5.pNext = &c->f6;
    c->f14.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES;
    c->f14.pNext = &c->f5;
    c->f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    c->f13.pNext = &c->f14;
    c->f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    c->f12.pNext = &c->f13;
    VkPhysicalDeviceFeatures2 feat2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &c->f12,
    };
    vkGetPhysicalDeviceFeatures2(phy, &feat2);

    vkGetPhysicalDeviceMemoryProperties(phy, &c->mem_props);
    c->has_uma_memory_type = khr_check_uma_memory(&c->mem_props);

    VkSemaphoreTypeCreateInfo sem_type_ci = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
    };
    VkPhysicalDeviceExternalSemaphoreInfo ext_sem_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
        .pNext = &sem_type_ci,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    VkExternalSemaphoreProperties ext_sem_props = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES,
    };
    vkGetPhysicalDeviceExternalSemaphoreProperties(phy, &ext_sem_info, &ext_sem_props);
    c->exportable_timeline = (ext_sem_props.externalSemaphoreFeatures &
                              VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT) != 0;

    khr_discover_queues(phy, &c->gfx_family, &c->compute_family, &c->xfer_family);

    return true;
}

int32_t khr_score_device(const khr_device_candidate_t* c, dev_t compositor_dev) {
    if (!c->f12.bufferDeviceAddress) return -1;
    if (!c->f12.timelineSemaphore) return -1;
    if (!c->f13.dynamicRendering) return -1;
    if (!c->f13.synchronization2) return -1;
    if (c->gfx_family == UINT32_MAX) return -1;
    if (!c->exportable_timeline) return -1;
    if (!c->has_ext_mem_fd || !c->has_ext_sem_fd ||
        !c->has_drm_modifier || !c->has_dma_buf) {
        return -1;
    }

    int32_t score = 0;

    /* DRM node matching with Wayland compositor */
    if (compositor_dev != 0) {
        if (c->render_dev == compositor_dev) {
            score += 100'000;
        } else if (c->primary_dev == compositor_dev) {
            score += 90'000;
        }
    }

    /* Prioritize Vulkan 1.4 core standard */
    if (c->is_vulkan_1_4) {
        score += 10'000;
    }

    /* Prioritize UMA Integrated GPUs for zero-copy CPU-GPU shared DRAM */
    if (c->props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
        score += 5'000;
    } else if (c->props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
        score += 4'000;
    } else if (c->props.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU) {
        score += 2'000;
    } else {
        score += 1'000;
    }

    if (c->has_uma_memory_type) {
        score += 5'000;
    }
    if (c->f14.maintenance5) {
        score += 2'000;
    }
    if (c->f14.hostImageCopy) {
        score += 2'000;
    }
    if (c->f14.pushDescriptor) {
        score += 1'000;
    }
    /* Vulkan 1.4 companion features, exercised by the offscreen frame path:
     * maintenance5 subresource queries, maintenance6 push-constants2 record,
     * host image copy readback. Zero in hand-built test candidates, so their
     * exact-score assertions are unaffected. */
    if (c->f5.maintenance5) {
        score += 750;
    }
    if (c->f6.maintenance6) {
        score += 750;
    }
    if (c->fhost.hostImageCopy) {
        score += 1'000;
    }
    if (c->xfer_family != c->gfx_family) {
        score += 500;
    }
    if (c->compute_family != c->gfx_family && c->compute_family != c->xfer_family) {
        score += 500;
    }

    return score;
}

void khr_gfx_device_lock_queues(khr_gfx_device_t* d) {
    if (d != nullptr && d->submit_mutex_live) {
        (void)pthread_mutex_lock(&d->submit_mutex);
    }
}

void khr_gfx_device_unlock_queues(khr_gfx_device_t* d) {
    if (d != nullptr && d->submit_mutex_live) {
        (void)pthread_mutex_unlock(&d->submit_mutex);
    }
}

void khr_gfx_device_wait_idle(khr_gfx_device_t* d) {
    if (d == nullptr || d->gfx_queue == VK_NULL_HANDLE) {
        return;
    }
    khr_gfx_device_lock_queues(d);
    (void)vkQueueWaitIdle(d->gfx_queue);
    khr_gfx_device_unlock_queues(d);
}

bool khr_gfx_device_init(khr_gfx_device_t* d, dev_t compositor_dev) {
    if (!d) return false;
    memset(d, 0, sizeof(*d));
    d->acquire_fd = -1;
    d->drm_fd = -1;
    if (pthread_mutex_init(&d->submit_mutex, nullptr) != 0) {
        return false;
    }
    d->submit_mutex_live = true;

    VkApplicationInfo app_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Khoros",
        .applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
        .pEngineName = "Khoros Engine",
        .engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
        .apiVersion = VK_API_VERSION_1_4,
    };
    VkInstanceCreateInfo instance_ci = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info,
    };
    if (vkCreateInstance(&instance_ci, nullptr, &d->instance) != VK_SUCCESS) {
        return false;
    }

    uint32_t phy_count = 0;
    if (vkEnumeratePhysicalDevices(d->instance, &phy_count, nullptr) != VK_SUCCESS || phy_count == 0) {
        khr_gfx_device_destroy(d);
        return false;
    }

    VkPhysicalDevice physical_devices[KHR_MAX_PHYSICAL_DEVICES];
    uint32_t num_devices = (phy_count > KHR_MAX_PHYSICAL_DEVICES) ? KHR_MAX_PHYSICAL_DEVICES : phy_count;
    if (vkEnumeratePhysicalDevices(d->instance, &num_devices, physical_devices) != VK_SUCCESS) {
        khr_gfx_device_destroy(d);
        return false;
    }

    int32_t best_score = -1;
    khr_device_candidate_t best_cand = {};
    bool found_candidate = false;

    for (uint32_t i = 0; i < num_devices; i++) {
        khr_device_candidate_t cand;
        khr_query_device_candidate(physical_devices[i], &cand);
        int32_t score = khr_score_device(&cand, compositor_dev);
        if (score > best_score) {
            best_score = score;
            best_cand = cand;
            found_candidate = true;
        }
    }

    if (!found_candidate || best_score < 0) {
        khr_gfx_device_destroy(d);
        return false;
    }

    d->phy = best_cand.phy;
    d->gfx_family = best_cand.gfx_family;
    d->xfer_family = best_cand.xfer_family;
    d->compute_family = best_cand.compute_family;
    d->render_dev = best_cand.render_dev;
    d->primary_dev = best_cand.primary_dev;
    d->has_host_image_copy = best_cand.f14.hostImageCopy || best_cand.fhost.hostImageCopy;
    d->has_push_descriptor = best_cand.f14.pushDescriptor;
    d->has_maintenance5 = best_cand.f5.maintenance5 == VK_TRUE;
    d->has_maintenance6 = best_cand.f6.maintenance6 == VK_TRUE;

    VkPhysicalDeviceVulkan14Properties p14 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_PROPERTIES,
    };
    VkPhysicalDeviceProperties2 props2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &p14,
    };
    vkGetPhysicalDeviceProperties2(d->phy, &props2);
    d->max_push_descriptors = p14.maxPushDescriptors;

    /* Companion 1.4-era features, chained only when the winning device
     * reports them (unknown sTypes would break creation on older drivers).
     * maintenance5: subresource layout queries; maintenance6: push-constants2
     * record path; host copy: queue-free image readback. */
    VkPhysicalDeviceHostImageCopyFeatures enableHost = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES,
    };
    VkPhysicalDeviceMaintenance6Features enableM6 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES,
    };
    VkPhysicalDeviceMaintenance5Features enableM5 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES,
    };
    void* feat_tail = nullptr;
    if (best_cand.is_vulkan_1_4 && best_cand.fhost.hostImageCopy) {
        enableHost.hostImageCopy = VK_TRUE;
        enableHost.pNext = feat_tail;
        feat_tail = &enableHost;
    }
    if (best_cand.is_vulkan_1_4 && best_cand.f6.maintenance6) {
        enableM6.maintenance6 = VK_TRUE;
        enableM6.pNext = feat_tail;
        feat_tail = &enableM6;
    }
    if (best_cand.is_vulkan_1_4 && best_cand.f5.maintenance5) {
        enableM5.maintenance5 = VK_TRUE;
        enableM5.pNext = feat_tail;
        feat_tail = &enableM5;
    }

    /* Logical Device Feature Chaining: 1.2 -> 1.3 -> 1.4 -> companions */
    VkPhysicalDeviceVulkan14Features enable14 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
        .pNext = feat_tail,
        .maintenance5 = best_cand.f14.maintenance5,
        .pushDescriptor = best_cand.f14.pushDescriptor,
        .hostImageCopy = best_cand.f14.hostImageCopy,
    };
    VkPhysicalDeviceVulkan13Features enable13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .pNext = &enable14,
        .dynamicRendering = VK_TRUE,
        .synchronization2 = VK_TRUE,
        .maintenance4 = best_cand.f13.maintenance4,
    };
    VkPhysicalDeviceVulkan12Features enable12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &enable13,
        .bufferDeviceAddress = VK_TRUE,
        .timelineSemaphore = VK_TRUE,
        .scalarBlockLayout = best_cand.f12.scalarBlockLayout,
    };

    /* Build unique queue creation infos (zero-heap de-duplication) */
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci[3];
    uint32_t num_qci = 0;

    qci[num_qci++] = (VkDeviceQueueCreateInfo){
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = d->gfx_family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };

    if (d->xfer_family != d->gfx_family) {
        qci[num_qci++] = (VkDeviceQueueCreateInfo){
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = d->xfer_family,
            .queueCount = 1,
            .pQueuePriorities = &priority,
        };
    }

    if (d->compute_family != d->gfx_family && d->compute_family != d->xfer_family) {
        qci[num_qci++] = (VkDeviceQueueCreateInfo){
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = d->compute_family,
            .queueCount = 1,
            .pQueuePriorities = &priority,
        };
    }

    /* Check for host memory import extension */
    uint32_t ext_count = 0;
    bool supports_host_import = false;
    if (vkEnumerateDeviceExtensionProperties(d->phy, nullptr, &ext_count, nullptr) == VK_SUCCESS && ext_count > 0) {
        constexpr uint32_t MAX_DEV_EXTS = 256;
        VkExtensionProperties exts[MAX_DEV_EXTS];
        uint32_t c_ext = (ext_count > MAX_DEV_EXTS) ? MAX_DEV_EXTS : ext_count;
        if (vkEnumerateDeviceExtensionProperties(d->phy, nullptr, &c_ext, exts) == VK_SUCCESS) {
            for (uint32_t i = 0; i < c_ext; i++) {
                if (strcmp(exts[i].extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME) == 0) {
                    supports_host_import = true;
                    break;
                }
            }
        }
    }

    const char* device_extensions[8];
    uint32_t num_exts = 0;
    device_extensions[num_exts++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
    device_extensions[num_exts++] = VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME;
    device_extensions[num_exts++] = VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME;
    device_extensions[num_exts++] = VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME;
    device_extensions[num_exts++] = VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME;
    if (supports_host_import) {
        device_extensions[num_exts++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    }
    /* EXT-suffixed host-copy entry points only resolve via ProcAddr when the
     * extension is enabled. Without it the frame silently falls back to the
     * transfer engine despite the feature bit being on (observed on HW). */
    if (best_cand.has_host_copy_ext) {
        device_extensions[num_exts++] = VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME;
    }

    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &enable12,
        .queueCreateInfoCount = num_qci,
        .pQueueCreateInfos = qci,
        .enabledExtensionCount = num_exts,
        .ppEnabledExtensionNames = device_extensions,
    };

    if (vkCreateDevice(d->phy, &dci, nullptr, &d->device) != VK_SUCCESS) {
        khr_gfx_device_destroy(d);
        return false;
    }

    vkGetDeviceQueue(d->device, d->gfx_family, 0, &d->gfx_queue);
    if (d->xfer_family != d->gfx_family) {
        vkGetDeviceQueue(d->device, d->xfer_family, 0, &d->xfer_queue);
    } else {
        d->xfer_queue = d->gfx_queue;
    }
    if (d->compute_family != d->gfx_family && d->compute_family != d->xfer_family) {
        vkGetDeviceQueue(d->device, d->compute_family, 0, &d->compute_queue);
    } else {
        d->compute_queue = d->gfx_queue;
    }

    d->vkGetMemoryFdKHR = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(d->device, "vkGetMemoryFdKHR");
    d->vkGetSemaphoreFdKHR = (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(d->device, "vkGetSemaphoreFdKHR");
    d->vkGetImageDrmFormatModifierPropertiesEXT =
        (PFN_vkGetImageDrmFormatModifierPropertiesEXT)vkGetDeviceProcAddr(
            d->device, "vkGetImageDrmFormatModifierPropertiesEXT");
    d->vkGetBufferDeviceAddress =
        (PFN_vkGetBufferDeviceAddress)vkGetDeviceProcAddr(d->device, "vkGetBufferDeviceAddress");
    if (supports_host_import) {
        d->vkGetMemoryHostPointerPropertiesEXT =
            (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(
                d->device, "vkGetMemoryHostPointerPropertiesEXT");
        d->has_external_memory_host = (d->vkGetMemoryHostPointerPropertiesEXT != nullptr);
    }

    if (!d->vkGetMemoryFdKHR || !d->vkGetSemaphoreFdKHR ||
        !d->vkGetImageDrmFormatModifierPropertiesEXT || !d->vkGetBufferDeviceAddress) {
        khr_gfx_device_destroy(d);
        return false;
    }

    VkCommandPoolCreateInfo pool_ci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = d->gfx_family,
    };
    if (vkCreateCommandPool(d->device, &pool_ci, nullptr, &d->pool) != VK_SUCCESS) {
        khr_gfx_device_destroy(d);
        return false;
    }

    /* Global Acquire Timeline Semaphore */
    VkExportSemaphoreCreateInfo exp_sem = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    VkSemaphoreTypeCreateInfo type_sem = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .pNext = &exp_sem,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = 0,
    };
    VkSemaphoreCreateInfo sem_ci = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &type_sem,
    };
    if (vkCreateSemaphore(d->device, &sem_ci, nullptr, &d->acquire_sem) != VK_SUCCESS) {
        khr_gfx_device_destroy(d);
        return false;
    }

    VkSemaphoreGetFdInfoKHR get_sem_fd = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
        .semaphore = d->acquire_sem,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    if (d->vkGetSemaphoreFdKHR(d->device, &get_sem_fd, &d->acquire_fd) != VK_SUCCESS || d->acquire_fd < 0) {
        khr_gfx_device_destroy(d);
        return false;
    }
    d->acquire_point = 0;

    d->drm_fd = khr_drm_open_render_node(d->render_dev);
    return true;
}

void khr_gfx_device_destroy(khr_gfx_device_t* d) {
    if (!d) return;

    if (d->submit_mutex_live) {
        (void)pthread_mutex_destroy(&d->submit_mutex);
        d->submit_mutex_live = false;
    }
    if (d->acquire_fd >= 0) {
        close(d->acquire_fd);
        d->acquire_fd = -1;
    }
    if (d->acquire_sem != VK_NULL_HANDLE && d->device != VK_NULL_HANDLE) {
        vkDestroySemaphore(d->device, d->acquire_sem, nullptr);
        d->acquire_sem = VK_NULL_HANDLE;
    }
    if (d->pool != VK_NULL_HANDLE && d->device != VK_NULL_HANDLE) {
        vkDestroyCommandPool(d->device, d->pool, nullptr);
        d->pool = VK_NULL_HANDLE;
    }
    if (d->device != VK_NULL_HANDLE) {
        vkDestroyDevice(d->device, nullptr);
        d->device = VK_NULL_HANDLE;
    }
    if (d->instance != VK_NULL_HANDLE) {
        vkDestroyInstance(d->instance, nullptr);
        d->instance = VK_NULL_HANDLE;
    }
    if (d->drm_fd >= 0) {
        close(d->drm_fd);
        d->drm_fd = -1;
    }
    memset(d, 0, sizeof(*d));
    d->acquire_fd = -1;
    d->drm_fd = -1;
}

VkDeviceAddress khr_gfx_get_buffer_address(const khr_gfx_device_t* d, VkBuffer buf) {
    if (!d || !d->device || !d->vkGetBufferDeviceAddress || buf == VK_NULL_HANDLE) {
        return 0;
    }
    VkBufferDeviceAddressInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = buf,
    };
    return d->vkGetBufferDeviceAddress(d->device, &info);
}

VkSampleCountFlagBits khr_gfx_sample_count(const khr_gfx_device_t* d) {
    if (d == nullptr || d->phy == VK_NULL_HANDLE) {
        return VK_SAMPLE_COUNT_1_BIT;
    }
    VkPhysicalDeviceProperties props = {};
    vkGetPhysicalDeviceProperties(d->phy, &props);
    VkSampleCountFlags f = props.limits.framebufferColorSampleCounts &
                           props.limits.framebufferDepthSampleCounts;
    if ((f & VK_SAMPLE_COUNT_4_BIT) != 0) {
        return VK_SAMPLE_COUNT_4_BIT;
    }
    if ((f & VK_SAMPLE_COUNT_2_BIT) != 0) {
        return VK_SAMPLE_COUNT_2_BIT;
    }
    return VK_SAMPLE_COUNT_1_BIT;
}

VkFormat khr_gfx_depth_format(const khr_gfx_device_t* d) {
    if (d == nullptr || d->phy == VK_NULL_HANDLE) {
        return VK_FORMAT_UNDEFINED;
    }
    const VkFormat try_fmt[2] = {
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D24_UNORM_S8_UINT,
    };
    for (uint32_t i = 0; i < 2; i++) {
        VkFormatProperties fp = {};
        vkGetPhysicalDeviceFormatProperties(d->phy, try_fmt[i], &fp);
        if ((fp.optimalTilingFeatures &
             VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
            return try_fmt[i];
        }
    }
    return VK_FORMAT_UNDEFINED;
}

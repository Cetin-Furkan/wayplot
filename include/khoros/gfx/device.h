#ifndef KHOROS_GFX_DEVICE_H
#define KHOROS_GFX_DEVICE_H

#if __STDC_VERSION__ < 202311L
#error "Khoros requires pure ISO C23 (compile with -std=c23)"
#endif

#ifdef _STDBOOL_H
#error "<stdbool.h> is strictly banned. In C23, bool, true, and false are native language keywords."
#endif

#ifdef _STDALIGN_H
#error "<stdalign.h> is strictly banned. In C23, alignas and alignof are native language keywords."
#endif

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <pthread.h>
#include <vulkan/vulkan.h>

constexpr uint32_t KHR_MAX_QUEUE_FAMILIES = 16;
constexpr uint32_t KHR_MAX_PHYSICAL_DEVICES = 16;

typedef struct {
    VkPhysicalDevice                          phy;
    VkPhysicalDeviceProperties                props;
    VkPhysicalDeviceVulkan12Features          f12;
    VkPhysicalDeviceVulkan13Features          f13;
    VkPhysicalDeviceVulkan14Features          f14;
    VkPhysicalDeviceMaintenance5Features      f5;
    VkPhysicalDeviceMaintenance6Features      f6;
    VkPhysicalDeviceHostImageCopyFeatures     fhost;
    VkPhysicalDeviceMemoryProperties mem_props;
    uint32_t                         gfx_family;
    uint32_t                         compute_family;
    uint32_t                         xfer_family;
    dev_t                            render_dev;
    dev_t                            primary_dev;
    bool                             has_drm_properties;
    bool                             is_vulkan_1_4;
    bool                             exportable_timeline;
    bool                             has_host_copy_ext;
    bool                             has_ext_mem_fd;
    bool                             has_ext_sem_fd;
    bool                             has_drm_modifier;
    bool                             has_dma_buf;
    bool                             has_uma_memory_type;
    int32_t                          score;
} khr_device_candidate_t;

typedef struct khr_gfx_device {
    VkInstance       instance;
    union {
        VkPhysicalDevice phy;
        VkPhysicalDevice physical_device;
    };
    VkDevice         device;

    union {
        VkQueue gfx_queue;
        VkQueue graphics_queue;
    };
    union {
        VkQueue xfer_queue;
        VkQueue transfer_queue;
    };
    VkQueue          compute_queue;

    union {
        uint32_t gfx_family;
        uint32_t graphics_family;
    };
    union {
        uint32_t xfer_family;
        uint32_t transfer_family;
    };
    uint32_t         compute_family;

    union {
        VkCommandPool pool;
        VkCommandPool cmd_pool;
    };

    /* Device timeline the GPU signals per frame. acquire_fd is an
     * OPAQUE_FD export of that semaphore when the driver allows it — not a
     * DRM syncobj fd. Present uses a separate native DRM timeline plus the
     * counter-query bridge in khr_dmabuf_present_sync(); this fd is the
     * mature-stack seam, unused on Xe. */
    VkSemaphore      acquire_sem;
    int              acquire_fd;
    uint64_t         acquire_point;

    /* DRM render node file descriptor */
    int              drm_fd;
    dev_t            render_dev;
    dev_t            primary_dev;

    /* Vulkan 1.4 & BDA Function Pointers */
    union {
        PFN_vkGetBufferDeviceAddress vkGetBufferDeviceAddress;
        PFN_vkGetBufferDeviceAddress pfn_vkGetBufferDeviceAddress;
    };
    union {
        PFN_vkGetMemoryFdKHR vkGetMemoryFdKHR;
        PFN_vkGetMemoryFdKHR pfn_vkGetMemoryFdKHR;
    };
    union {
        PFN_vkGetSemaphoreFdKHR vkGetSemaphoreFdKHR;
        PFN_vkGetSemaphoreFdKHR pfn_vkGetSemaphoreFdKHR;
    };
    union {
        PFN_vkGetImageDrmFormatModifierPropertiesEXT vkGetImageDrmFormatModifierPropertiesEXT;
        PFN_vkGetImageDrmFormatModifierPropertiesEXT pfn_vkGetImageDrmFormatModifierPropertiesEXT;
    };
    union {
        PFN_vkGetMemoryHostPointerPropertiesEXT vkGetMemoryHostPointerPropertiesEXT;
        PFN_vkGetMemoryHostPointerPropertiesEXT pfn_vkGetMemoryHostPointerPropertiesEXT;
    };

    /* Capabilities */
    bool             has_external_memory_host;
    bool             has_host_image_copy;
    bool             has_push_descriptor;
    bool             has_maintenance5;
    bool             has_maintenance6;
    uint32_t         max_push_descriptors;

    /* Queue submission is externally synchronized by the Vulkan spec: on
     * integrated GPUs gfx/compute/xfer resolve to one VkQueue handle, so
     * every vkQueueSubmit2 path serializes here. Uncontended today (only the
     * frame loop submits), mandatory the moment Core 1 submits. */
    pthread_mutex_t  submit_mutex;
    bool             submit_mutex_live;
} khr_gfx_device_t;

void khr_gfx_device_lock_queues(khr_gfx_device_t* d);

void khr_gfx_device_unlock_queues(khr_gfx_device_t* d);

[[nodiscard]]
bool khr_gfx_device_init(khr_gfx_device_t* d, dev_t compositor_dev);

void khr_gfx_device_destroy(khr_gfx_device_t* d);

[[nodiscard]]
VkDeviceAddress khr_gfx_get_buffer_address(const khr_gfx_device_t* d, VkBuffer buf);

[[nodiscard]]
int32_t khr_score_device(const khr_device_candidate_t* c, dev_t compositor_dev);

#endif /* KHOROS_GFX_DEVICE_H */

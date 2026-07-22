#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <wayland-client.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_wayland.h>

#include "xdg-shell-client-protocol.h"

// ============================================================================
// Types and Constants
// ============================================================================

static const char *APP_NAME           = "WayPlot Engine";
static const char *ENGINE_NAME        = "WayPlot Graphics Core";
constexpr uint32_t APP_VERSION        = VK_MAKE_API_VERSION(0, 1, 0, 0);
constexpr uint32_t ENGINE_VERSION     = VK_MAKE_API_VERSION(0, 1, 0, 0);
constexpr uint32_t TARGET_VK_VERSION  = VK_API_VERSION_1_4;
constexpr float DEFAULT_QUEUE_PRIO    = 1.0f;

constexpr uint32_t DEFAULT_WIDTH      = 800;
constexpr uint32_t DEFAULT_HEIGHT     = 600;

typedef enum {
    FEATURE_CHECK_ACCEPT_ALL = 0,
    FEATURE_CHECK_REJECT_SOME,
    FEATURE_CHECK_REJECT_ALL
} FeatureCheckStatus;

struct EngineRequestedFeatures {
    VkPhysicalDeviceFeatures base;
    VkPhysicalDeviceVulkan12Features v12;
    VkPhysicalDeviceVulkan13Features v13;
    VkPhysicalDeviceVulkan14Features v14;
};

struct WaylandContext {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct xdg_wm_base *xdg_wm_base;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
    uint32_t width;
    uint32_t height;
    bool configured;
    bool running;
};

struct VulkanSwapchain {
    VkSwapchainKHR handle;
    VkFormat image_format;
    VkColorSpaceKHR color_space;
    VkExtent2D extent;
    uint32_t image_count;
    VkImage *images;
    VkImageView *image_views;
};

struct VulkanContext {
    VkInstance instance;
    VkSurfaceKHR surface;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue graphics_queue;
    uint32_t graphics_queue_family;
    struct VulkanSwapchain swapchain;

    VkPipelineLayout pipeline_layout;
    VkPipeline graphics_pipeline;

    VkCommandPool command_pool;
    VkCommandBuffer command_buffer;
    VkSemaphore image_available_semaphore;
    VkSemaphore render_finished_semaphore;
    VkFence in_flight_fence;
};

struct Application {
    struct WaylandContext wl;
    struct VulkanContext vk;
};

// ============================================================================
// Wayland Callbacks
// ============================================================================

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *shell, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(shell, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = xdg_wm_base_ping,
};

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial) {
    struct Application *app = data;
    xdg_surface_ack_configure(xdg_surface, serial);
    app->wl.configured = true;

    if (app->wl.surface != nullptr) {
        wl_surface_commit(app->wl.surface);
    }
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void xdg_toplevel_configure(void *data, struct xdg_toplevel *xdg_toplevel,
                                    int32_t width, int32_t height, struct wl_array *states) {
    (void)data;
    (void)xdg_toplevel;
    (void)states;
    struct Application *app = data;

    if (width > 0 && height > 0) {
        app->wl.width = (uint32_t)width;
        app->wl.height = (uint32_t)height;
    }
}

static void xdg_toplevel_close(void *data, struct xdg_toplevel *xdg_toplevel) {
    (void)xdg_toplevel;
    struct Application *app = data;
    app->wl.running = false;
}

static void xdg_toplevel_configure_bounds(void *data, struct xdg_toplevel *xdg_toplevel,
                                           int32_t width, int32_t height) {
    (void)data;
    (void)xdg_toplevel;
    (void)width;
    (void)height;
}

static void xdg_toplevel_wm_capabilities(void *data, struct xdg_toplevel *xdg_toplevel,
                                         struct wl_array *capabilities) {
    (void)data;
    (void)xdg_toplevel;
    (void)capabilities;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_configure,
    .close = xdg_toplevel_close,
    .configure_bounds = xdg_toplevel_configure_bounds,
    .wm_capabilities = xdg_toplevel_wm_capabilities,
};

static void registry_handler(void *data, struct wl_registry *registry, uint32_t id,
                             const char *interface, uint32_t version) {
    (void)version;
    struct Application *app = data;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        app->wl.compositor = wl_registry_bind(registry, id, &wl_compositor_interface, 4);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        app->wl.xdg_wm_base = wl_registry_bind(registry, id, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(app->wl.xdg_wm_base, &xdg_wm_base_listener, app);
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_handler,
    .global_remove = nullptr,
};

// ============================================================================
// Wayland Connection
// ============================================================================

static bool wayland_init(struct Application *app) {
    app->wl.running = true;
    app->wl.configured = false;
    app->wl.width = DEFAULT_WIDTH;
    app->wl.height = DEFAULT_HEIGHT;

    app->wl.display = wl_display_connect(nullptr);
    if (app->wl.display == nullptr) {
        fprintf(stderr, "Error: Failed to connect to Wayland display server.\n");
        return false;
    }

    app->wl.registry = wl_display_get_registry(app->wl.display);
    wl_registry_add_listener(app->wl.registry, &registry_listener, app);
    wl_display_roundtrip(app->wl.display);

    if (app->wl.compositor == nullptr || app->wl.xdg_wm_base == nullptr) {
        fprintf(stderr, "Error: Compositor missing required xdg-shell interfaces.\n");
        return false;
    }

    app->wl.surface = wl_compositor_create_surface(app->wl.compositor);
    app->wl.xdg_surface = xdg_wm_base_get_xdg_surface(app->wl.xdg_wm_base, app->wl.surface);
    xdg_surface_add_listener(app->wl.xdg_surface, &xdg_surface_listener, app);

    app->wl.xdg_toplevel = xdg_surface_get_toplevel(app->wl.xdg_surface);
    xdg_toplevel_add_listener(app->wl.xdg_toplevel, &xdg_toplevel_listener, app);
    xdg_toplevel_set_title(app->wl.xdg_toplevel, "WayPlot Engine");

    wl_surface_commit(app->wl.surface);
    wl_display_roundtrip(app->wl.display);

    printf("Wayland connection and surface initialized.\n");
    return true;
}

// ============================================================================
// Vulkan Feature Check & Core Setup
// ============================================================================

static FeatureCheckStatus check_gpu_features(VkPhysicalDevice gpu, const struct EngineRequestedFeatures *requested) {
    VkPhysicalDeviceVulkan14Features supported14 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
        .pNext = nullptr
    };

    VkPhysicalDeviceVulkan13Features supported13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .pNext = &supported14
    };

    VkPhysicalDeviceVulkan12Features supported12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &supported13
    };

    VkPhysicalDeviceFeatures2 supported2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &supported12
    };

    vkGetPhysicalDeviceFeatures2(gpu, &supported2);

    uint32_t total_requested = 0;
    uint32_t total_rejected = 0;

    #define AUDIT_FLAG(req_val, supp_val, name) \
        if ((req_val) == VK_TRUE) { \
            total_requested++; \
            if ((supp_val) != VK_TRUE) { \
                fprintf(stderr, "  [REJECTED] Feature '%s' requested but NOT supported by GPU.\n", name); \
                total_rejected++; \
            } \
        }

    AUDIT_FLAG(requested->base.samplerAnisotropy, supported2.features.samplerAnisotropy, "Sampler Anisotropy");
    AUDIT_FLAG(requested->base.fillModeNonSolid, supported2.features.fillModeNonSolid, "Wireframe Fill Mode");
    AUDIT_FLAG(requested->v12.timelineSemaphore, supported12.timelineSemaphore, "Timeline Semaphores");
    AUDIT_FLAG(requested->v12.bufferDeviceAddress, supported12.bufferDeviceAddress, "Buffer Device Address");
    AUDIT_FLAG(requested->v12.descriptorIndexing, supported12.descriptorIndexing, "Descriptor Indexing");
    AUDIT_FLAG(requested->v13.dynamicRendering, supported13.dynamicRendering, "Dynamic Rendering");
    AUDIT_FLAG(requested->v13.synchronization2, supported13.synchronization2, "Synchronization2");
    AUDIT_FLAG(requested->v14.pushDescriptor, supported14.pushDescriptor, "Push Descriptors");
    AUDIT_FLAG(requested->v14.hostImageCopy, supported14.hostImageCopy, "Host Image Copy");
    AUDIT_FLAG(requested->v14.maintenance5, supported14.maintenance5, "Maintenance 5");
    AUDIT_FLAG(requested->v14.maintenance6, supported14.maintenance6, "Maintenance 6");

    #undef AUDIT_FLAG

    if (total_rejected == 0) {
        return FEATURE_CHECK_ACCEPT_ALL;
    } else if (total_rejected == total_requested) {
        return FEATURE_CHECK_REJECT_ALL;
    } else {
        return FEATURE_CHECK_REJECT_SOME;
    }
}

static bool create_instance(VkInstance *out_instance) {
    const char *extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME
    };

    const VkApplicationInfo app_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pNext = nullptr,
        .pApplicationName = APP_NAME,
        .applicationVersion = APP_VERSION,
        .pEngineName = ENGINE_NAME,
        .engineVersion = ENGINE_VERSION,
        .apiVersion = TARGET_VK_VERSION,
    };

    const VkInstanceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .pApplicationInfo = &app_info,
        .enabledLayerCount = 0,
        .ppEnabledLayerNames = nullptr,
        .enabledExtensionCount = 2,
        .ppEnabledExtensionNames = extensions,
    };

    if (vkCreateInstance(&create_info, nullptr, out_instance) != VK_SUCCESS) {
        fprintf(stderr, "Error: vkCreateInstance failed.\n");
        return false;
    }

    printf("Vulkan 1.4 Instance created.\n");
    return true;
}

static bool create_wayland_surface(struct Application *app) {
    const VkWaylandSurfaceCreateInfoKHR create_info = {
        .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
        .pNext = nullptr,
        .flags = 0,
        .display = app->wl.display,
        .surface = app->wl.surface,
    };

    if (vkCreateWaylandSurfaceKHR(app->vk.instance, &create_info, nullptr, &app->vk.surface) != VK_SUCCESS) {
        fprintf(stderr, "Error: Failed to create VkWaylandSurfaceKHR.\n");
        return false;
    }

    printf("Vulkan surface created.\n");
    return true;
}

static bool select_gpu_and_verify(struct Application *app, const struct EngineRequestedFeatures *req_features) {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(app->vk.instance, &count, nullptr);

    if (count == 0) {
        fprintf(stderr, "Error: No physical Vulkan GPUs found.\n");
        return false;
    }

    VkPhysicalDevice gpus[count];
    vkEnumeratePhysicalDevices(app->vk.instance, &count, gpus);

    for (uint32_t i = 0; i < count; ++i) {
        if (check_gpu_features(gpus[i], req_features) != FEATURE_CHECK_ACCEPT_ALL) {
            continue;
        }

        uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpus[i], &family_count, nullptr);

        VkQueueFamilyProperties families[family_count];
        vkGetPhysicalDeviceQueueFamilyProperties(gpus[i], &family_count, families);

        for (uint32_t q = 0; q < family_count; ++q) {
            if (families[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                VkBool32 present_support = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(gpus[i], q, app->vk.surface, &present_support);

                if (present_support == VK_TRUE) {
                    app->vk.physical_device = gpus[i];
                    app->vk.graphics_queue_family = q;

                    VkPhysicalDeviceProperties props;
                    vkGetPhysicalDeviceProperties(gpus[i], &props);
                    printf("Selected GPU: %s\n", props.deviceName);
                    return true;
                }
            }
        }
    }

    fprintf(stderr, "Error: No GPU met feature and presentation criteria.\n");
    return false;
}

static bool create_device(struct Application *app, const struct EngineRequestedFeatures *req_features) {
    const float queue_priority = DEFAULT_QUEUE_PRIO;
    const VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .queueFamilyIndex = app->vk.graphics_queue_family,
        .queueCount = 1,
        .pQueuePriorities = &queue_priority,
    };

    const char *device_extensions[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME
    };

    VkPhysicalDeviceVulkan14Features enable_v14 = req_features->v14;
    enable_v14.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES;
    enable_v14.pNext = nullptr;

    VkPhysicalDeviceVulkan13Features enable_v13 = req_features->v13;
    enable_v13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    enable_v13.pNext = &enable_v14;

    VkPhysicalDeviceVulkan12Features enable_v12 = req_features->v12;
    enable_v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    enable_v12.pNext = &enable_v13;

    const VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &enable_v12,
        .flags = 0,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledLayerCount = 0,
        .ppEnabledLayerNames = nullptr,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = device_extensions,
        .pEnabledFeatures = &req_features->base,
    };

    if (vkCreateDevice(app->vk.physical_device, &device_info, nullptr, &app->vk.device) != VK_SUCCESS) {
        fprintf(stderr, "Error: vkCreateDevice failed.\n");
        return false;
    }

    vkGetDeviceQueue(app->vk.device, app->vk.graphics_queue_family, 0, &app->vk.graphics_queue);
    printf("Logical VkDevice created.\n");
    return true;
}

// ============================================================================
// Swapchain Setup
// ============================================================================

static bool create_swapchain(struct Application *app) {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(app->vk.physical_device, app->vk.surface, &caps);

    uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(app->vk.physical_device, app->vk.surface, &format_count, nullptr);
    VkSurfaceFormatKHR formats[format_count];
    vkGetPhysicalDeviceSurfaceFormatsKHR(app->vk.physical_device, app->vk.surface, &format_count, formats);

    VkSurfaceFormatKHR chosen_format = formats[0];
    for (uint32_t i = 0; i < format_count; ++i) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_SRGB &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen_format = formats[i];
            break;
        }
    }

    VkExtent2D extent = {
        .width = app->wl.width,
        .height = app->wl.height,
    };

    if (caps.currentExtent.width != UINT32_MAX) {
        extent = caps.currentExtent;
    }

    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) {
        image_count = caps.maxImageCount;
    }

    const VkSwapchainCreateInfoKHR create_info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .pNext = nullptr,
        .flags = 0,
        .surface = app->vk.surface,
        .minImageCount = image_count,
        .imageFormat = chosen_format.format,
        .imageColorSpace = chosen_format.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
        .oldSwapchain = VK_NULL_HANDLE,
    };

    if (vkCreateSwapchainKHR(app->vk.device, &create_info, nullptr, &app->vk.swapchain.handle) != VK_SUCCESS) {
        fprintf(stderr, "Error: vkCreateSwapchainKHR failed.\n");
        return false;
    }

    app->vk.swapchain.image_format = chosen_format.format;
    app->vk.swapchain.color_space = chosen_format.colorSpace;
    app->vk.swapchain.extent = extent;

    vkGetSwapchainImagesKHR(app->vk.device, app->vk.swapchain.handle, &app->vk.swapchain.image_count, nullptr);
    app->vk.swapchain.images = malloc(sizeof(VkImage) * app->vk.swapchain.image_count);
    vkGetSwapchainImagesKHR(app->vk.device, app->vk.swapchain.handle, &app->vk.swapchain.image_count, app->vk.swapchain.images);

    app->vk.swapchain.image_views = malloc(sizeof(VkImageView) * app->vk.swapchain.image_count);

    for (uint32_t i = 0; i < app->vk.swapchain.image_count; ++i) {
        const VkImageViewCreateInfo view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .image = app->vk.swapchain.images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = app->vk.swapchain.image_format,
            .components = {
                .r = VK_COMPONENT_SWIZZLE_IDENTITY,
                .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                .a = VK_COMPONENT_SWIZZLE_IDENTITY,
            },
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        };

        if (vkCreateImageView(app->vk.device, &view_info, nullptr, &app->vk.swapchain.image_views[i]) != VK_SUCCESS) {
            fprintf(stderr, "Error: vkCreateImageView failed for index %u.\n", i);
            return false;
        }
    }

    printf("Swapchain created successfully (%u images, %ux%u resolution).\n",
           app->vk.swapchain.image_count, extent.width, extent.height);
    return true;
}

static void destroy_swapchain(struct Application *app) {
    if (app->vk.swapchain.image_views != nullptr) {
        for (uint32_t i = 0; i < app->vk.swapchain.image_count; ++i) {
            vkDestroyImageView(app->vk.device, app->vk.swapchain.image_views[i], nullptr);
        }
        free(app->vk.swapchain.image_views);
        app->vk.swapchain.image_views = nullptr;
    }

    if (app->vk.swapchain.images != nullptr) {
        free(app->vk.swapchain.images);
        app->vk.swapchain.images = nullptr;
    }

    if (app->vk.swapchain.handle != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(app->vk.device, app->vk.swapchain.handle, nullptr);
        app->vk.swapchain.handle = VK_NULL_HANDLE;
    }
}

// ============================================================================
// Pipeline Setup
// ============================================================================

static bool load_shader_module(VkDevice device, const char *filepath, VkShaderModule *out_module) {
    FILE *file = fopen(filepath, "rb");
    if (!file) {
        fprintf(stderr, "Error: Could not open shader file '%s'. Did you run 'glslc'?\n", filepath);
        return false;
    }

    fseek(file, 0, SEEK_END);
    long filesize = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (filesize <= 0 || (filesize % 4) != 0) {
        fprintf(stderr, "Error: Invalid SPIR-V file size for '%s'.\n", filepath);
        fclose(file);
        return false;
    }

    uint32_t *buffer = malloc(filesize);
    if (fread(buffer, 1, filesize, file) != (size_t)filesize) {
        fprintf(stderr, "Error: Failed to read shader file '%s'.\n", filepath);
        free(buffer);
        fclose(file);
        return false;
    }
    fclose(file);

    const VkShaderModuleCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .codeSize = filesize,
        .pCode = buffer,
    };

    VkResult res = vkCreateShaderModule(device, &create_info, nullptr, out_module);
    free(buffer);

    if (res != VK_SUCCESS) {
        fprintf(stderr, "Error: vkCreateShaderModule failed for '%s' (%d).\n", filepath, res);
        return false;
    }
    return true;
}

static bool create_graphics_pipeline(struct Application *app) {
    VkShaderModule vert_module = VK_NULL_HANDLE;
    VkShaderModule frag_module = VK_NULL_HANDLE;

    if (!load_shader_module(app->vk.device, "vert.spv", &vert_module) ||
        !load_shader_module(app->vk.device, "frag.spv", &frag_module)) {
        return false;
    }

    const VkPipelineShaderStageCreateInfo shader_stages[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = nullptr,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vert_module,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = nullptr,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = frag_module,
            .pName = "main",
        }
    };

    const VkPipelineVertexInputStateCreateInfo vertex_input_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .pNext = nullptr,
        .vertexBindingDescriptionCount = 0,
        .vertexAttributeDescriptionCount = 0,
    };

    const VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .pNext = nullptr,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, // 2 distinct lines (4 vertices)
        .primitiveRestartEnable = VK_FALSE,
    };

    const VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .pNext = nullptr,
        .viewportCount = 1,
        .scissorCount = 1,
    };

    const VkPipelineRasterizationStateCreateInfo rasterizer = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .pNext = nullptr,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depthBiasEnable = VK_FALSE,
        .lineWidth = 1.0f,
    };

    const VkPipelineMultisampleStateCreateInfo multisampling = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .pNext = nullptr,
        .sampleShadingEnable = VK_FALSE,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    const VkPipelineColorBlendAttachmentState color_blend_attachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        .blendEnable = VK_FALSE,
    };

    const VkPipelineColorBlendStateCreateInfo color_blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .pNext = nullptr,
        .logicOpEnable = VK_FALSE,
        .attachmentCount = 1,
        .pAttachments = &color_blend_attachment,
    };

    const VkDynamicState dynamic_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };

    const VkPipelineDynamicStateCreateInfo dynamic_state_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .pNext = nullptr,
        .dynamicStateCount = 2,
        .pDynamicStates = dynamic_states,
    };

    const VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .setLayoutCount = 0,
        .pushConstantRangeCount = 0,
    };

    if (vkCreatePipelineLayout(app->vk.device, &pipeline_layout_info, nullptr, &app->vk.pipeline_layout) != VK_SUCCESS) {
        fprintf(stderr, "Error: vkCreatePipelineLayout failed.\n");
        vkDestroyShaderModule(app->vk.device, vert_module, nullptr);
        vkDestroyShaderModule(app->vk.device, frag_module, nullptr);
        return false;
    }

    // Vulkan 1.3 Dynamic Rendering target format link
    const VkPipelineRenderingCreateInfo rendering_create_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .pNext = nullptr,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &app->vk.swapchain.image_format,
    };

    const VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering_create_info,
        .stageCount = 2,
        .pStages = shader_stages,
        .pVertexInputState = &vertex_input_info,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = nullptr,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_state_info,
        .layout = app->vk.pipeline_layout,
        .renderPass = VK_NULL_HANDLE, // No legacy render pass!
        .subpass = 0,
    };

    if (vkCreateGraphicsPipelines(app->vk.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &app->vk.graphics_pipeline) != VK_SUCCESS) {
        fprintf(stderr, "Error: vkCreateGraphicsPipelines failed.\n");
        vkDestroyPipelineLayout(app->vk.device, app->vk.pipeline_layout, nullptr);
        vkDestroyShaderModule(app->vk.device, vert_module, nullptr);
        vkDestroyShaderModule(app->vk.device, frag_module, nullptr);
        return false;
    }

    vkDestroyShaderModule(app->vk.device, vert_module, nullptr);
    vkDestroyShaderModule(app->vk.device, frag_module, nullptr);

    printf("Graphics pipeline created successfully.\n");
    return true;
}

// ============================================================================
// Command Pool, Sync & Frame Drawing
// ============================================================================

static bool create_commands_and_sync(struct Application *app) {
    const VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = app->vk.graphics_queue_family,
    };

    if (vkCreateCommandPool(app->vk.device, &pool_info, nullptr, &app->vk.command_pool) != VK_SUCCESS) {
        fprintf(stderr, "Error: vkCreateCommandPool failed.\n");
        return false;
    }

    const VkCommandBufferAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext = nullptr,
        .commandPool = app->vk.command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };

    if (vkAllocateCommandBuffers(app->vk.device, &alloc_info, &app->vk.command_buffer) != VK_SUCCESS) {
        fprintf(stderr, "Error: vkAllocateCommandBuffers failed.\n");
        return false;
    }

    const VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
    };

    const VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };

    if (vkCreateSemaphore(app->vk.device, &semaphore_info, nullptr, &app->vk.image_available_semaphore) != VK_SUCCESS ||
        vkCreateSemaphore(app->vk.device, &semaphore_info, nullptr, &app->vk.render_finished_semaphore) != VK_SUCCESS ||
        vkCreateFence(app->vk.device, &fence_info, nullptr, &app->vk.in_flight_fence) != VK_SUCCESS) {
        fprintf(stderr, "Error: Failed to create sync objects.\n");
        return false;
    }

    return true;
}

static void draw_frame(struct Application *app) {
    vkWaitForFences(app->vk.device, 1, &app->vk.in_flight_fence, VK_TRUE, UINT64_MAX);
    vkResetFences(app->vk.device, 1, &app->vk.in_flight_fence);

    uint32_t image_index = 0;
    VkResult acquire_res = vkAcquireNextImageKHR(app->vk.device, app->vk.swapchain.handle, UINT64_MAX,
                                                 app->vk.image_available_semaphore, VK_NULL_HANDLE, &image_index);

    if (acquire_res != VK_SUCCESS && acquire_res != VK_SUBOPTIMAL_KHR) {
        fprintf(stderr, "Error: vkAcquireNextImageKHR failed (%d).\n", acquire_res);
        return;
    }

    vkResetCommandBuffer(app->vk.command_buffer, 0);

    const VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = 0,
        .pInheritanceInfo = nullptr,
    };

    vkBeginCommandBuffer(app->vk.command_buffer, &begin_info);

    // Transition image layout to COLOR_ATTACHMENT_OPTIMAL
    const VkImageMemoryBarrier2 barrier_to_attach = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .srcAccessMask = 0,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = app->vk.swapchain.images[image_index],
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };

    const VkDependencyInfo dep_info_attach = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pNext = nullptr,
        .dependencyFlags = 0,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier_to_attach,
    };

    vkCmdPipelineBarrier2(app->vk.command_buffer, &dep_info_attach);

    // Clear background to Dark Slate Grey (0.1, 0.12, 0.15)
    const VkClearValue clear_color = {{{0.1f, 0.12f, 0.15f, 1.0f}}};

    const VkRenderingAttachmentInfo color_attachment = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .pNext = nullptr,
        .imageView = app->vk.swapchain.image_views[image_index],
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .resolveMode = VK_RESOLVE_MODE_NONE,
        .resolveImageView = VK_NULL_HANDLE,
        .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = clear_color,
    };

    const VkRenderingInfo rendering_info = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .pNext = nullptr,
        .flags = 0,
        .renderArea = {
            .offset = {0, 0},
            .extent = app->vk.swapchain.extent,
        },
        .layerCount = 1,
        .viewMask = 0,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_attachment,
        .pDepthAttachment = nullptr,
        .pStencilAttachment = nullptr,
    };

    vkCmdBeginRendering(app->vk.command_buffer, &rendering_info);

    // Bind Graphics Pipeline
    vkCmdBindPipeline(app->vk.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, app->vk.graphics_pipeline);

    // Set Dynamic Viewport & Scissor
    const VkViewport viewport = {
        .x = 0.0f,
        .y = 0.0f,
        .width = (float)app->vk.swapchain.extent.width,
        .height = (float)app->vk.swapchain.extent.height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    vkCmdSetViewport(app->vk.command_buffer, 0, 1, &viewport);

    const VkRect2D scissor = {
        .offset = {0, 0},
        .extent = app->vk.swapchain.extent,
    };
    vkCmdSetScissor(app->vk.command_buffer, 0, 1, &scissor);

    vkCmdDraw(app->vk.command_buffer, 3, 1, 0, 0);

    vkCmdEndRendering(app->vk.command_buffer);

    // Transition image layout to PRESENT_SRC_KHR
    const VkImageMemoryBarrier2 barrier_to_present = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
        .dstAccessMask = 0,
        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = app->vk.swapchain.images[image_index],
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };

    const VkDependencyInfo dep_info_present = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pNext = nullptr,
        .dependencyFlags = 0,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier_to_present,
    };

    vkCmdPipelineBarrier2(app->vk.command_buffer, &dep_info_present);
    vkEndCommandBuffer(app->vk.command_buffer);

    // Submit Work
    const VkPipelineStageFlags wait_stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &app->vk.image_available_semaphore,
        .pWaitDstStageMask = &wait_stages,
        .commandBufferCount = 1,
        .pCommandBuffers = &app->vk.command_buffer,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &app->vk.render_finished_semaphore,
    };

    vkQueueSubmit(app->vk.graphics_queue, 1, &submit_info, app->vk.in_flight_fence);

    // Present Frame to Wayland Window
    const VkPresentInfoKHR present_info = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = nullptr,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &app->vk.render_finished_semaphore,
        .swapchainCount = 1,
        .pSwapchains = &app->vk.swapchain.handle,
        .pImageIndices = &image_index,
        .pResults = nullptr,
    };

    VkResult present_res = vkQueuePresentKHR(app->vk.graphics_queue, &present_info);
    if (present_res != VK_SUCCESS && present_res != VK_SUBOPTIMAL_KHR) {
        fprintf(stderr, "Error: vkQueuePresentKHR failed (%d).\n", present_res);
    }

    wl_display_flush(app->wl.display);
}

// ============================================================================
// Application Management & Event Loop
// ============================================================================

bool application_init(struct Application *app, const struct EngineRequestedFeatures *req_features) {
    if (!wayland_init(app)) return false;
    if (!create_instance(&app->vk.instance)) return false;
    if (!create_wayland_surface(app)) return false;
    if (!select_gpu_and_verify(app, req_features)) return false;
    if (!create_device(app, req_features)) return false;
    if (!create_swapchain(app)) return false;
    if (!create_commands_and_sync(app)) return false;
    if (!create_graphics_pipeline(app)) return false;

    return true;
}

void application_run(struct Application *app) {
    printf("Rendering initial frame with white plus symbol (+)...\n");

    // Render 1 frame to draw the plus symbol and map the window
    draw_frame(app);

    printf("Window rendered! Sleeping in event loop (0.0%% CPU / GPU)...\n");

    while (app->wl.running) {
        if (wl_display_dispatch(app->wl.display) == -1) {
            fprintf(stderr, "Wayland display connection lost.\n");
            break;
        }
    }
}

void application_cleanup(struct Application *app) {
    if (app->vk.device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(app->vk.device);

        if (app->vk.graphics_pipeline) vkDestroyPipeline(app->vk.device, app->vk.graphics_pipeline, nullptr);
        if (app->vk.pipeline_layout) vkDestroyPipelineLayout(app->vk.device, app->vk.pipeline_layout, nullptr);

        if (app->vk.image_available_semaphore) vkDestroySemaphore(app->vk.device, app->vk.image_available_semaphore, nullptr);
        if (app->vk.render_finished_semaphore) vkDestroySemaphore(app->vk.device, app->vk.render_finished_semaphore, nullptr);
        if (app->vk.in_flight_fence) vkDestroyFence(app->vk.device, app->vk.in_flight_fence, nullptr);
        if (app->vk.command_pool) vkDestroyCommandPool(app->vk.device, app->vk.command_pool, nullptr);
    }

    destroy_swapchain(app);

    if (app->vk.device != nullptr) {
        vkDestroyDevice(app->vk.device, nullptr);
    }
    if (app->vk.surface != nullptr) {
        vkDestroySurfaceKHR(app->vk.instance, app->vk.surface, nullptr);
    }
    if (app->vk.instance != nullptr) {
        vkDestroyInstance(app->vk.instance, nullptr);
    }
    if (app->wl.xdg_toplevel != nullptr) {
        xdg_toplevel_destroy(app->wl.xdg_toplevel);
    }
    if (app->wl.xdg_surface != nullptr) {
        xdg_surface_destroy(app->wl.xdg_surface);
    }
    if (app->wl.surface != nullptr) {
        wl_surface_destroy(app->wl.surface);
    }
    if (app->wl.xdg_wm_base != nullptr) {
        xdg_wm_base_destroy(app->wl.xdg_wm_base);
    }
    if (app->wl.compositor != nullptr) {
        wl_compositor_destroy(app->wl.compositor);
    }
    if (app->wl.registry != nullptr) {
        wl_registry_destroy(app->wl.registry);
    }
    if (app->wl.display != nullptr) {
        wl_display_disconnect(app->wl.display);
    }
    printf("Application cleanly destroyed.\n");
}

int main(void) {
    struct Application app = {0};

    const struct EngineRequestedFeatures requested_features = {
        .base = {
            .samplerAnisotropy = VK_TRUE,
            .fillModeNonSolid  = VK_TRUE,
        },
        .v12 = {
            .timelineSemaphore   = VK_TRUE,
            .bufferDeviceAddress = VK_TRUE,
            .descriptorIndexing  = VK_TRUE,
        },
        .v13 = {
            .dynamicRendering    = VK_TRUE,
            .synchronization2     = VK_TRUE,
        },
        .v14 = {
            .pushDescriptor      = VK_TRUE,
            .hostImageCopy       = VK_TRUE,
            .maintenance5        = VK_TRUE,
            .maintenance6        = VK_TRUE,
        }
    };

    if (!application_init(&app, &requested_features)) {
        fprintf(stderr, "Error: Initialization failed.\n");
        application_cleanup(&app);
        return EXIT_FAILURE;
    }

    application_run(&app);
    application_cleanup(&app);
    return EXIT_SUCCESS;
}

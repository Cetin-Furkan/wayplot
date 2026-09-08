#include "test_framework.h"
#include "khoros/gfx/device.h"
#include "khoros/gfx/pipeline.h"
#include "khoros/gfx/bda_arena.h"
#include "khoros/uring/ring.h"
#include <vulkan/vulkan.h>
#include <string.h>
#include <sys/sysmacros.h>

/* Push constant layout contract: strictly <= 128 bytes */
typedef struct {
    uint64_t vertex_bda;       /* 8 bytes */
    uint64_t uniform_bda;      /* 8 bytes */
    uint64_t index_bda;        /* 8 bytes */
    uint32_t viewport_w;       /* 4 bytes */
    uint32_t viewport_h;       /* 4 bytes */
    uint32_t frame_index;      /* 4 bytes */
    uint32_t flags;            /* 4 bytes */
    float    mvp[16];          /* 64 bytes */
    uint32_t padding[6];       /* 24 bytes -> Total: 128 bytes */
} khr_bda_push_constants_t;

static_assert(sizeof(khr_bda_push_constants_t) == 128, "Push constant layout must not exceed 128 bytes");
static_assert(alignof(khr_bda_push_constants_t) == 8, "Push constants must have 8-byte BDA alignment");

/* First physical device or false when the host has no Vulkan driver/GPU.
 * Callers TEST_ASSERT the result: GPU-less CI records a clean FAIL instead
 * of feeding a null handle into the loader and aborting the whole runner. */
static bool khr_test_first_device(VkInstance instance, VkPhysicalDevice* out_dev) {
    if (out_dev == nullptr) {
        return false;
    }
    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS || count == 0) {
        return false;
    }
    VkPhysicalDevice devs[8] = {};
    uint32_t n = (count > 8) ? 8 : count;
    if (vkEnumeratePhysicalDevices(instance, &n, devs) != VK_SUCCESS || n == 0) {
        return false;
    }
    *out_dev = devs[0];
    return true;
}

[[nodiscard]]
bool test_vulkan_instance_1_4_support(void) {
    uint32_t api_ver = 0;
    VkResult res = vkEnumerateInstanceVersion(&api_ver);
    TEST_ASSERT_EQ(res, VK_SUCCESS, "vkEnumerateInstanceVersion failed");
    TEST_ASSERT(api_ver >= VK_API_VERSION_1_4, "Vulkan instance version must be >= 1.4");

    /* Create pure Vulkan 1.4 instance with zero legacy validation errors */
    VkApplicationInfo app_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Khoros Engine Headless Test",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "Khoros",
        .engineVersion = VK_MAKE_VERSION(1, 4, 0),
        .apiVersion = VK_API_VERSION_1_4,
    };

    VkInstanceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info,
    };

    VkInstance instance = VK_NULL_HANDLE;
    res = vkCreateInstance(&create_info, nullptr, &instance);
    TEST_ASSERT_EQ(res, VK_SUCCESS, "Failed to create Vulkan 1.4 instance");
    TEST_ASSERT_NOT_NULL(instance, "Instance handle must not be null");

    vkDestroyInstance(instance, nullptr);
    return true;
}

[[nodiscard]]
bool test_vulkan_physical_device_enumeration(void) {
    VkApplicationInfo app_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .apiVersion = VK_API_VERSION_1_4,
    };
    VkInstanceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info,
    };

    VkInstance instance = VK_NULL_HANDLE;
    TEST_ASSERT_EQ(vkCreateInstance(&create_info, nullptr, &instance), VK_SUCCESS, "Instance create");

    uint32_t dev_count = 0;
    TEST_ASSERT_EQ(vkEnumeratePhysicalDevices(instance, &dev_count, nullptr), VK_SUCCESS, "Enum devices count");
    TEST_ASSERT(dev_count > 0, "Expected at least 1 Vulkan physical device");

    VkPhysicalDevice devices[16] = {};
    if (dev_count > 16) dev_count = 16;
    TEST_ASSERT_EQ(vkEnumeratePhysicalDevices(instance, &dev_count, devices), VK_SUCCESS, "Enum devices");

    for (uint32_t i = 0; i < dev_count; i++) {
        VkPhysicalDeviceProperties props = {};
        vkGetPhysicalDeviceProperties(devices[i], &props);
        TEST_ASSERT(props.apiVersion >= VK_API_VERSION_1_3, "Physical device apiVersion must be >= 1.3");
        TEST_ASSERT(props.limits.maxPushConstantsSize >= 128, "GPU must support >= 128 bytes push constants");
    }

    vkDestroyInstance(instance, nullptr);
    return true;
}

[[nodiscard]]
bool test_vulkan_physical_device_scoring(void) {
    VkApplicationInfo app_info = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_4 };
    VkInstanceCreateInfo create_info = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app_info };
    VkInstance instance = VK_NULL_HANDLE;
    TEST_ASSERT_EQ(vkCreateInstance(&create_info, nullptr, &instance), VK_SUCCESS, "Instance create");

    uint32_t dev_count = 0;
    vkEnumeratePhysicalDevices(instance, &dev_count, nullptr);
    VkPhysicalDevice devices[8] = {};
    if (dev_count > 8) dev_count = 8;
    vkEnumeratePhysicalDevices(instance, &dev_count, devices);

    int best_score = -1;
    VkPhysicalDevice best_dev = VK_NULL_HANDLE;

    for (uint32_t i = 0; i < dev_count; i++) {
        VkPhysicalDeviceProperties props = {};
        vkGetPhysicalDeviceProperties(devices[i], &props);

        int score = 0;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score += 1000;
        else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score += 500;

        VkPhysicalDeviceVulkan12Features v12 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        };
        VkPhysicalDeviceVulkan13Features v13 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
            .pNext = &v12,
        };
        VkPhysicalDeviceFeatures2 feat2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext = &v13,
        };

        vkGetPhysicalDeviceFeatures2(devices[i], &feat2);
        if (v12.bufferDeviceAddress == VK_TRUE) score += 300;
        if (v13.dynamicRendering == VK_TRUE) score += 200;
        if (v13.synchronization2 == VK_TRUE) score += 200;

        if (score > best_score) {
            best_score = score;
            best_dev = devices[i];
        }
    }

    TEST_ASSERT(best_score >= 500, "Best device must score at least 500 (integrated or discrete GPU)");
    TEST_ASSERT_NOT_NULL(best_dev, "Selected physical device must not be null");

    vkDestroyInstance(instance, nullptr);
    return true;
}

[[nodiscard]]
bool test_vulkan_bda_features_query(void) {
    VkApplicationInfo app_info = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_4 };
    VkInstanceCreateInfo create_info = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app_info };
    VkInstance instance = VK_NULL_HANDLE;
    TEST_ASSERT_EQ(vkCreateInstance(&create_info, nullptr, &instance), VK_SUCCESS, "Instance create");

    VkPhysicalDevice dev = VK_NULL_HANDLE;
    TEST_ASSERT(khr_test_first_device(instance, &dev), "no Vulkan physical device on this host");

    VkPhysicalDeviceVulkan12Features v12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
    };
    VkPhysicalDeviceFeatures2 feat2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &v12,
    };

    vkGetPhysicalDeviceFeatures2(dev, &feat2);
    TEST_ASSERT(v12.bufferDeviceAddress == VK_TRUE, "Vulkan 1.4 device MUST support Buffer Device Address (BDA)");
    TEST_ASSERT(v12.timelineSemaphore == VK_TRUE, "Vulkan 1.4 device MUST support Timeline Semaphores");

    vkDestroyInstance(instance, nullptr);
    return true;
}

[[nodiscard]]
bool test_vulkan_dynamic_rendering_and_sync2_features(void) {
    VkApplicationInfo app_info = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_4 };
    VkInstanceCreateInfo create_info = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app_info };
    VkInstance instance = VK_NULL_HANDLE;
    TEST_ASSERT_EQ(vkCreateInstance(&create_info, nullptr, &instance), VK_SUCCESS, "Instance create");

    VkPhysicalDevice dev = VK_NULL_HANDLE;
    TEST_ASSERT(khr_test_first_device(instance, &dev), "no Vulkan physical device on this host");

    VkPhysicalDeviceVulkan13Features v13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
    };
    VkPhysicalDeviceFeatures2 feat2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &v13,
    };

    vkGetPhysicalDeviceFeatures2(dev, &feat2);
    TEST_ASSERT(v13.dynamicRendering == VK_TRUE, "Device must support Vulkan 1.3 Dynamic Rendering");
    TEST_ASSERT(v13.synchronization2 == VK_TRUE, "Device must support Synchronization 2");

    vkDestroyInstance(instance, nullptr);
    return true;
}

[[nodiscard]]
bool test_vulkan_push_constant_layout_128b_contract(void) {
    khr_bda_push_constants_t pc = {};
    TEST_ASSERT_EQ(sizeof(pc), 128U, "Push constants must strictly equal 128 bytes");
    TEST_ASSERT_EQ(alignof(khr_bda_push_constants_t), 8U, "Push constants must be 8-byte aligned");

    /* Ensure BDA fields are 8-byte aligned within the struct */
    TEST_ASSERT_EQ(offsetof(khr_bda_push_constants_t, vertex_bda) % 8, 0U, "vertex_bda offset 8-byte alignment");
    TEST_ASSERT_EQ(offsetof(khr_bda_push_constants_t, uniform_bda) % 8, 0U, "uniform_bda offset 8-byte alignment");
    TEST_ASSERT_EQ(offsetof(khr_bda_push_constants_t, index_bda) % 8, 0U, "index_bda offset 8-byte alignment");

    /* Test pack and unpack */
    pc.vertex_bda = 0xDEADBEEF00001000ULL;
    pc.uniform_bda = 0xCAFEBABE00002000ULL;
    pc.viewport_w = 1920;
    pc.viewport_h = 1080;
    pc.frame_index = 42;

    uint8_t raw[128] = {};
    memcpy(raw, &pc, sizeof(pc));

    khr_bda_push_constants_t unpacked = {};
    memcpy(&unpacked, raw, sizeof(unpacked));

    TEST_ASSERT_EQ(unpacked.vertex_bda, 0xDEADBEEF00001000ULL, "vertex_bda roundtrip");
    TEST_ASSERT_EQ(unpacked.uniform_bda, 0xCAFEBABE00002000ULL, "uniform_bda roundtrip");
    TEST_ASSERT_EQ(unpacked.viewport_w, 1920U, "viewport_w roundtrip");
    TEST_ASSERT_EQ(unpacked.viewport_h, 1080U, "viewport_h roundtrip");
    TEST_ASSERT_EQ(unpacked.frame_index, 42U, "frame_index roundtrip");

    return true;
}

[[nodiscard]]
bool test_vulkan_uma_memory_types_query(void) {
    VkApplicationInfo app_info = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_4 };
    VkInstanceCreateInfo create_info = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app_info };
    VkInstance instance = VK_NULL_HANDLE;
    TEST_ASSERT_EQ(vkCreateInstance(&create_info, nullptr, &instance), VK_SUCCESS, "Instance create");

    VkPhysicalDevice dev = VK_NULL_HANDLE;
    TEST_ASSERT(khr_test_first_device(instance, &dev), "no Vulkan physical device on this host");

    VkPhysicalDeviceMemoryProperties mem_props = {};
    vkGetPhysicalDeviceMemoryProperties(dev, &mem_props);
    TEST_ASSERT(mem_props.memoryTypeCount > 0, "Device must report memory types");

    /* Verify presence of HOST_VISIBLE | HOST_COHERENT memory for direct UMA mapping */
    bool found_host_coherent = false;
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        VkMemoryPropertyFlags flags = mem_props.memoryTypes[i].propertyFlags;
        if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            found_host_coherent = true;
            break;
        }
    }
    TEST_ASSERT(found_host_coherent, "Physical device must support HOST_VISIBLE | HOST_COHERENT memory for UMA zero-copy");

    vkDestroyInstance(instance, nullptr);
    return true;
}

[[nodiscard]]
bool test_vulkan_queue_family_selection(void) {
    VkApplicationInfo app_info = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_4 };
    VkInstanceCreateInfo create_info = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app_info };
    VkInstance instance = VK_NULL_HANDLE;
    TEST_ASSERT_EQ(vkCreateInstance(&create_info, nullptr, &instance), VK_SUCCESS, "Instance create");

    VkPhysicalDevice dev = VK_NULL_HANDLE;
    TEST_ASSERT(khr_test_first_device(instance, &dev), "no Vulkan physical device on this host");

    uint32_t qf_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &qf_count, nullptr);
    TEST_ASSERT(qf_count > 0, "Device must have at least 1 queue family");

    VkQueueFamilyProperties qf_props[16] = {};
    if (qf_count > 16) qf_count = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &qf_count, qf_props);

    int graphics_idx = -1;
    int compute_idx = -1;

    for (uint32_t i = 0; i < qf_count; i++) {
        if (qf_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            if (graphics_idx == -1) graphics_idx = (int)i;
        }
        if (qf_props[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            if (compute_idx == -1) compute_idx = (int)i;
        }
    }

    TEST_ASSERT(graphics_idx >= 0, "Physical device must provide a Graphics queue family");
    TEST_ASSERT(compute_idx >= 0, "Physical device must provide a Compute queue family");

    vkDestroyInstance(instance, nullptr);
    return true;
}

/* ========================================================================= */
/* Milestone 1: Vulkan 1.4 Core Device, Pipeline & BDA Headless Unit Tests  */
/* ========================================================================= */

[[nodiscard]]
bool test_vulkan_device_init_and_scoring(void) {
    khr_gfx_device_t dev = {};
    /* Verify initialization with compositor_dev == 0 (headless / CI mode) */
    TEST_ASSERT(khr_gfx_device_init(&dev, (dev_t)0), "khr_gfx_device_init with compositor_dev == 0 must succeed");
    TEST_ASSERT_NOT_NULL(dev.instance, "instance must not be NULL");
    TEST_ASSERT_NOT_NULL(dev.device, "device must not be NULL");
    TEST_ASSERT_NOT_NULL(dev.gfx_queue, "gfx_queue must not be NULL");
    TEST_ASSERT_NOT_NULL(dev.xfer_queue, "xfer_queue must not be NULL");
    TEST_ASSERT_NOT_NULL(dev.compute_queue, "compute_queue must not be NULL");
    TEST_ASSERT_NOT_NULL(dev.pool, "command pool must not be NULL");

    /* Verify timeline semaphore export */
    TEST_ASSERT_NOT_NULL(dev.acquire_sem, "acquire_sem must not be NULL");
    TEST_ASSERT(dev.acquire_fd >= 0, "acquire_fd must be a valid file descriptor");
    TEST_ASSERT_EQ(dev.acquire_point, 0U, "initial acquire_point must be 0");

    /* Verify loaded function pointers */
    TEST_ASSERT_NOT_NULL(dev.vkGetBufferDeviceAddress, "vkGetBufferDeviceAddress must be loaded");
    TEST_ASSERT_NOT_NULL(dev.vkGetMemoryFdKHR, "vkGetMemoryFdKHR must be loaded");
    TEST_ASSERT_NOT_NULL(dev.vkGetSemaphoreFdKHR, "vkGetSemaphoreFdKHR must be loaded");
    TEST_ASSERT_NOT_NULL(dev.vkGetImageDrmFormatModifierPropertiesEXT, "vkGetImageDrmFormatModifierPropertiesEXT must be loaded");

    /* Test scoring functionality */
    khr_device_candidate_t cand = {
        .is_vulkan_1_4 = true,
        .exportable_timeline = true,
        .has_ext_mem_fd = true,
        .has_ext_sem_fd = true,
        .has_drm_modifier = true,
        .has_dma_buf = true,
        .has_uma_memory_type = true,
        .gfx_family = 0,
        .xfer_family = 0,
        .compute_family = 0,
        .render_dev = makedev(226, 128),
        .primary_dev = makedev(226, 0),
        .props = {
            .deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
        },
        .f12 = {
            .bufferDeviceAddress = VK_TRUE,
            .timelineSemaphore = VK_TRUE,
        },
        .f13 = {
            .dynamicRendering = VK_TRUE,
            .synchronization2 = VK_TRUE,
        },
        .f14 = {
            .maintenance5 = VK_TRUE,
        },
    };

    int32_t score_headless = khr_score_device(&cand, (dev_t)0);
    TEST_ASSERT(score_headless > 0, "Score without compositor matching must be positive");

    int32_t score_matched = khr_score_device(&cand, cand.render_dev);
    TEST_ASSERT_EQ(score_matched, score_headless + 100'000, "DRM render node match must add 100,000 points");

    int32_t score_primary = khr_score_device(&cand, cand.primary_dev);
    TEST_ASSERT_EQ(score_primary, score_headless + 90'000, "DRM primary node match must add 90,000 points");

    khr_gfx_device_destroy(&dev);
    TEST_ASSERT_NULL(dev.device, "device must be NULL after destroy");
    TEST_ASSERT_EQ(dev.acquire_fd, -1, "acquire_fd must be -1 after destroy");
    return true;
}

[[nodiscard]]
bool test_vulkan_bda_arena_alloc_and_coherence(void) {
    khr_gfx_device_t dev = {};
    TEST_ASSERT(khr_gfx_device_init(&dev, (dev_t)0), "khr_gfx_device_init failed");

    khr_bda_arena_t arena = {};
    constexpr size_t TEST_ARENA_SZ = 64 * 1024;
    TEST_ASSERT(khr_bda_arena_init(&dev, &arena, TEST_ARENA_SZ), "khr_bda_arena_init failed");
    TEST_ASSERT_NOT_NULL(arena.host_ptr, "host_ptr must be valid mapped address");
    TEST_ASSERT(arena.gpu_address != 0, "gpu_address must be non-zero 64-bit BDA");
    TEST_ASSERT_EQ(arena.gpu_address % 8, 0U, "gpu_address must be 8-byte aligned");
    TEST_ASSERT(arena.size >= TEST_ARENA_SZ, "arena size must be at least requested size");

    /* Verify address and host pointer offset calculations */
    VkDeviceAddress addr_off = khr_bda_arena_address_at(&arena, 128);
    TEST_ASSERT_EQ(addr_off, arena.gpu_address + 128, "address_at offset check");
    void* host_off = khr_bda_arena_host_at(&arena, 128);
    TEST_ASSERT_EQ(host_off, (void*)((uint8_t*)arena.host_ptr + 128), "host_at offset check");

    /* Verify bump allocator within arena */
    void* alloc1_host = nullptr;
    VkDeviceAddress alloc1_gpu = 0;
    TEST_ASSERT(khr_bda_arena_alloc(&arena, 256, 64, &alloc1_host, &alloc1_gpu), "khr_bda_arena_alloc block 1");
    TEST_ASSERT_NOT_NULL(alloc1_host, "alloc1_host not null");
    TEST_ASSERT_EQ((uintptr_t)alloc1_host % 64, 0U, "alloc1_host 64-byte alignment");
    TEST_ASSERT_EQ(alloc1_gpu % 64, 0U, "alloc1_gpu 64-byte alignment");

    void* alloc2_host = nullptr;
    VkDeviceAddress alloc2_gpu = 0;
    TEST_ASSERT(khr_bda_arena_alloc(&arena, 512, 16, &alloc2_host, &alloc2_gpu), "khr_bda_arena_alloc block 2");
    TEST_ASSERT_NOT_NULL(alloc2_host, "alloc2_host not null");
    TEST_ASSERT(alloc2_gpu > alloc1_gpu, "alloc2_gpu > alloc1_gpu");

    /* Test CPU-GPU writeback and coherence */
    uint32_t* p = (uint32_t*)arena.host_ptr;
    p[0] = 0xA1B2C3D4;
    p[1] = 0x5E6F7A8B;
    TEST_ASSERT_EQ(p[0], 0xA1B2C3D4U, "Host coherent readback word 0");
    TEST_ASSERT_EQ(p[1], 0x5E6F7A8BU, "Host coherent readback word 1");

    khr_bda_arena_reset(&arena);
    TEST_ASSERT_EQ(arena.head, 0U, "arena head must be reset to 0");

    khr_bda_arena_destroy(&dev, &arena);
    TEST_ASSERT_NULL(arena.host_ptr, "host_ptr must be null after destroy");
    TEST_ASSERT_EQ(arena.gpu_address, 0U, "gpu_address must be 0 after destroy");

    khr_gfx_device_destroy(&dev);
    return true;
}

[[nodiscard]]
bool test_vulkan_slang_bytecode_and_modules(void) {
    khr_shader_bytecode_t cv = khr_shader_get_card_vert();
    khr_shader_bytecode_t cf = khr_shader_get_card_frag();
    khr_shader_bytecode_t pv = khr_shader_get_plot_vert();
    khr_shader_bytecode_t pf = khr_shader_get_plot_frag();
    khr_shader_bytecode_t mv = khr_shader_get_mesh_vert();
    khr_shader_bytecode_t mf = khr_shader_get_mesh_frag();

    TEST_ASSERT_NOT_NULL(cv.code, "card.vert bytecode exists");
    TEST_ASSERT(cv.size_bytes > 0, "card.vert bytecode size > 0");
    TEST_ASSERT_EQ(cv.size_bytes % 4, 0U, "card.vert bytecode 4-byte aligned size");

    TEST_ASSERT_NOT_NULL(cf.code, "card.frag bytecode exists");
    TEST_ASSERT(cf.size_bytes > 0, "card.frag bytecode size > 0");
    TEST_ASSERT_EQ(cf.size_bytes % 4, 0U, "card.frag bytecode 4-byte aligned size");

    TEST_ASSERT_NOT_NULL(pv.code, "plot.vert bytecode exists");
    TEST_ASSERT(pv.size_bytes > 0, "plot.vert bytecode size > 0");
    TEST_ASSERT_EQ(pv.size_bytes % 4, 0U, "plot.vert bytecode 4-byte aligned size");

    TEST_ASSERT_NOT_NULL(pf.code, "plot.frag bytecode exists");
    TEST_ASSERT(pf.size_bytes > 0, "plot.frag bytecode size > 0");
    TEST_ASSERT_EQ(pf.size_bytes % 4, 0U, "plot.frag bytecode 4-byte aligned size");

    TEST_ASSERT_NOT_NULL(mv.code, "mesh.vert bytecode exists");
    TEST_ASSERT(mv.size_bytes > 0 && mv.size_bytes % 4 == 0, "mesh.vert bytecode");
    TEST_ASSERT_NOT_NULL(mf.code, "mesh.frag bytecode exists");
    TEST_ASSERT(mf.size_bytes > 0 && mf.size_bytes % 4 == 0, "mesh.frag bytecode");

    /* Validate SPIR-V 1.6 Magic Number (0x07230203) */
    constexpr uint32_t SPIRV_MAGIC = 0x07230203;
    TEST_ASSERT_EQ(cv.code[0], SPIRV_MAGIC, "card.vert SPIR-V magic");
    TEST_ASSERT_EQ(cf.code[0], SPIRV_MAGIC, "card.frag SPIR-V magic");
    TEST_ASSERT_EQ(pv.code[0], SPIRV_MAGIC, "plot.vert SPIR-V magic");
    TEST_ASSERT_EQ(pf.code[0], SPIRV_MAGIC, "plot.frag SPIR-V magic");
    TEST_ASSERT_EQ(mv.code[0], SPIRV_MAGIC, "mesh.vert SPIR-V magic");
    TEST_ASSERT_EQ(mf.code[0], SPIRV_MAGIC, "mesh.frag SPIR-V magic");

    /* Validate 4-byte pointer alignment for Vulkan spec compliance */
    TEST_ASSERT_EQ((uintptr_t)cv.code % 4, 0U, "card.vert 4-byte pointer alignment");
    TEST_ASSERT_EQ((uintptr_t)cf.code % 4, 0U, "card.frag 4-byte pointer alignment");
    TEST_ASSERT_EQ((uintptr_t)pv.code % 4, 0U, "plot.vert 4-byte pointer alignment");
    TEST_ASSERT_EQ((uintptr_t)pf.code % 4, 0U, "plot.frag 4-byte pointer alignment");

    /* Test Vulkan VkShaderModule creation */
    khr_gfx_device_t dev = {};
    TEST_ASSERT(khr_gfx_device_init(&dev, (dev_t)0), "device init");

    VkShaderModule mod = VK_NULL_HANDLE;
    TEST_ASSERT(khr_shader_module_create(dev.device, cv.code, cv.size_bytes, &mod), "create card.vert module");
    TEST_ASSERT_NOT_NULL(mod, "card.vert module handle not null");
    khr_shader_module_destroy(dev.device, mod);

    TEST_ASSERT(khr_shader_module_create(dev.device, cf.code, cf.size_bytes, &mod), "create card.frag module");
    khr_shader_module_destroy(dev.device, mod);

    TEST_ASSERT(khr_shader_module_create(dev.device, pv.code, pv.size_bytes, &mod), "create plot.vert module");
    khr_shader_module_destroy(dev.device, mod);

    TEST_ASSERT(khr_shader_module_create(dev.device, pf.code, pf.size_bytes, &mod), "create plot.frag module");
    khr_shader_module_destroy(dev.device, mod);

    TEST_ASSERT(khr_shader_module_create(dev.device, mv.code, mv.size_bytes, &mod), "create mesh.vert module");
    khr_shader_module_destroy(dev.device, mod);
    TEST_ASSERT(khr_shader_module_create(dev.device, mf.code, mf.size_bytes, &mod), "create mesh.frag module");
    khr_shader_module_destroy(dev.device, mod);

    khr_gfx_device_destroy(&dev);
    return true;
}

[[nodiscard]]
bool test_vulkan_dynamic_rendering_pipeline_create(void) {
    khr_gfx_device_t dev = {};
    TEST_ASSERT(khr_gfx_device_init(&dev, (dev_t)0), "device init");

    /* Create Card Dynamic Rendering Pipeline */
    khr_card_pipeline_t card_pipe = {};
    TEST_ASSERT(khr_card_pipeline_init(&card_pipe, &dev, VK_FORMAT_B8G8R8A8_SRGB), "khr_card_pipeline_init");
    TEST_ASSERT_NOT_NULL(card_pipe.pipeline, "card pipeline handle not null");
    TEST_ASSERT_NOT_NULL(card_pipe.layout, "card pipeline layout not null");

    /* Create Plot Dynamic Rendering Pipeline */
    khr_plot_pipeline_t plot_pipe = {};
    TEST_ASSERT(khr_plot_pipeline_init(&plot_pipe, &dev, VK_FORMAT_B8G8R8A8_SRGB), "khr_plot_pipeline_init");
    TEST_ASSERT_NOT_NULL(plot_pipe.pipeline, "plot pipeline handle not null");
    TEST_ASSERT_NOT_NULL(plot_pipe.layout, "plot pipeline layout not null");

    khr_mesh_pipeline_t mesh_pipe = {};
    TEST_ASSERT(khr_mesh_pipeline_init(&mesh_pipe, &dev, VK_FORMAT_B8G8R8A8_SRGB), "khr_mesh_pipeline_init");
    TEST_ASSERT_NOT_NULL(mesh_pipe.pipeline, "mesh pipeline handle not null");

    /* Test recording draw commands into command buffer */
    VkCommandBufferAllocateInfo cb_ai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = dev.pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    TEST_ASSERT_EQ(vkAllocateCommandBuffers(dev.device, &cb_ai, &cmd), VK_SUCCESS, "allocate command buffer");

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    TEST_ASSERT_EQ(vkBeginCommandBuffer(cmd, &begin_info), VK_SUCCESS, "begin command buffer");

    khr_card_push_t card_push = {
        .cards_addr = 0x1000'0000ULL,
        .screen_extent = { 1920.0f, 1080.0f },
        .scale = 1.0f,
        .card_index = 0,
    };
    khr_card_draw(&card_pipe, cmd, &card_push, 4);

    khr_plot_push_t plot_push = {
        .samples_addr = 0x2000'0000ULL,
        .count = 256,
        .amp = 1.0f,
        .half_w = 0.05f,
    };
    khr_plot_draw(&plot_pipe, cmd, &plot_push, 255);

    khr_mesh_push_t mesh_push = {
        .verts_addr = 0x3000'0000ULL,
        .indices_addr = 0x3000'1000ULL,
        .index_count = 36,
        .vert_count = 8,
    };
    khr_mesh_draw(&mesh_pipe, cmd, &mesh_push);

    TEST_ASSERT_EQ(vkEndCommandBuffer(cmd), VK_SUCCESS, "end command buffer");

    vkFreeCommandBuffers(dev.device, dev.pool, 1, &cmd);

    khr_card_pipeline_destroy(&card_pipe);
    khr_mesh_pipeline_destroy(&mesh_pipe);
    TEST_ASSERT_NULL(card_pipe.pipeline, "card pipeline null after destroy");

    khr_plot_pipeline_destroy(&plot_pipe);
    TEST_ASSERT_NULL(plot_pipe.pipeline, "plot pipeline null after destroy");

    khr_gfx_device_destroy(&dev);
    return true;
}

[[nodiscard]]
bool test_bda_arena_uring_registration(void) {
    khr_gfx_device_t dev = {};
    TEST_ASSERT(khr_gfx_device_init(&dev, (dev_t)0), "device init");

    khr_bda_arena_t arena = {};
    TEST_ASSERT(khr_bda_arena_init(&dev, &arena, KHR_BDA_DEFAULT_ARENA_SZ), "arena init");

    khr_uring_config_t cfg_b = khr_uring_config_ring_b();
    khr_uring_t ring_b = {};
    TEST_ASSERT(khr_uring_init(&ring_b, &cfg_b), "Ring B init");

    /* If arena was imported from host hugepage memory, io_uring buffer registration succeeds */
    if (arena.is_imported) {
        TEST_ASSERT(khr_bda_arena_register_ring(&arena, &ring_b), "khr_bda_arena_register_ring");
        TEST_ASSERT(arena.registered_with_uring, "registered_with_uring flag must be true");

        khr_bda_arena_unregister_ring(&arena, &ring_b);
        TEST_ASSERT(!arena.registered_with_uring, "registered_with_uring flag must be false");
    }

    khr_uring_destroy(&ring_b);
    khr_bda_arena_destroy(&dev, &arena);
    khr_gfx_device_destroy(&dev);
    return true;
}

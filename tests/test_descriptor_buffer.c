#include "test_framework.h"
#include "khoros/gfx/device.h"
#include "khoros/gfx/descriptor_buffer.h"
#include <vulkan/vulkan.h>
#include <string.h>

[[nodiscard]]
bool test_descriptor_buffer_device_support(void) {
    khr_gfx_device_t dev = {};
    if (!khr_gfx_device_init(&dev, (dev_t)0)) {
        return false;
    }

    /* On Intel Xe Linux driver (Mesa ANV), VK_EXT_descriptor_buffer is supported */
    TEST_ASSERT(dev.has_descriptor_buffer, "VK_EXT_descriptor_buffer must be supported and enabled");
    TEST_ASSERT(dev.descriptor_buffer_props.samplerDescriptorSize > 0,
                "Sampler descriptor size must be non-zero");
    TEST_ASSERT(dev.descriptor_buffer_props.sampledImageDescriptorSize > 0,
                "Sampled image descriptor size must be non-zero");
    TEST_ASSERT(dev.descriptor_buffer_props.descriptorBufferOffsetAlignment > 0,
                "Descriptor buffer offset alignment must be non-zero");

    TEST_ASSERT(dev.vkGetDescriptorSetLayoutSizeEXT != nullptr,
                "vkGetDescriptorSetLayoutSizeEXT must be loaded");
    TEST_ASSERT(dev.vkGetDescriptorSetLayoutBindingOffsetEXT != nullptr,
                "vkGetDescriptorSetLayoutBindingOffsetEXT must be loaded");
    TEST_ASSERT(dev.vkGetDescriptorEXT != nullptr,
                "vkGetDescriptorEXT must be loaded");
    TEST_ASSERT(dev.vkCmdBindDescriptorBuffersEXT != nullptr,
                "vkCmdBindDescriptorBuffersEXT must be loaded");
    TEST_ASSERT(dev.vkCmdSetDescriptorBufferOffsetsEXT != nullptr,
                "vkCmdSetDescriptorBufferOffsetsEXT must be loaded");

    khr_gfx_device_destroy(&dev);
    return true;
}

[[nodiscard]]
bool test_descriptor_heap_lifecycle_and_alloc(void) {
    khr_gfx_device_t dev = {};
    if (!khr_gfx_device_init(&dev, (dev_t)0)) {
        return false;
    }

    khr_descriptor_heap_t heap = {};
    constexpr uint32_t MAX_TEX = 64;
    constexpr uint32_t MAX_SAMP = 16;
    bool init_ok = khr_descriptor_heap_init(&heap, &dev, MAX_TEX, MAX_SAMP);
    TEST_ASSERT(init_ok, "khr_descriptor_heap_init must succeed");

    TEST_ASSERT_NE(heap.resource_buffer, VK_NULL_HANDLE, "Resource buffer must be valid");
    TEST_ASSERT_NE(heap.sampler_buffer, VK_NULL_HANDLE, "Sampler buffer must be valid");
    TEST_ASSERT_NOT_NULL(heap.resource_host_map, "Resource host map must be non-null");
    TEST_ASSERT_NOT_NULL(heap.sampler_host_map, "Sampler host map must be non-null");
    TEST_ASSERT(heap.resource_bda != 0, "Resource BDA must be non-zero");
    TEST_ASSERT(heap.sampler_bda != 0, "Sampler BDA must be non-zero");

    TEST_ASSERT_EQ(khr_descriptor_heap_get_resource_bda(&heap), heap.resource_bda,
                   "Accessor must match resource BDA");
    TEST_ASSERT_EQ(khr_descriptor_heap_get_sampler_bda(&heap), heap.sampler_bda,
                   "Accessor must match sampler BDA");

    TEST_ASSERT_EQ(heap.max_textures, MAX_TEX, "Max textures must match configuration");
    TEST_ASSERT_EQ(heap.max_samplers, MAX_SAMP, "Max samplers must match configuration");
    TEST_ASSERT_EQ(heap.texture_count, 0U, "Initial texture count must be 0");
    TEST_ASSERT_EQ(heap.sampler_count, 0U, "Initial sampler count must be 0");

    khr_descriptor_heap_destroy(&heap);
    TEST_ASSERT_EQ(heap.resource_buffer, VK_NULL_HANDLE, "Resource buffer must be destroyed");
    TEST_ASSERT_EQ(heap.sampler_buffer, VK_NULL_HANDLE, "Sampler buffer must be destroyed");
    TEST_ASSERT_NULL(heap.resource_host_map, "Resource host map must be null after destroy");
    TEST_ASSERT_NULL(heap.sampler_host_map, "Sampler host map must be null after destroy");

    khr_gfx_device_destroy(&dev);
    return true;
}

[[nodiscard]]
bool test_descriptor_heap_register_sampler_direct_memory(void) {
    khr_gfx_device_t dev = {};
    if (!khr_gfx_device_init(&dev, (dev_t)0)) {
        return false;
    }

    khr_descriptor_heap_t heap = {};
    TEST_ASSERT(khr_descriptor_heap_init(&heap, &dev, 16, 4), "heap init failed");

    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
    };
    VkSampler sampler1 = VK_NULL_HANDLE;
    TEST_ASSERT_EQ(vkCreateSampler(dev.device, &sci, nullptr, &sampler1), VK_SUCCESS,
                   "vkCreateSampler 1 failed");

    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
    VkSampler sampler2 = VK_NULL_HANDLE;
    TEST_ASSERT_EQ(vkCreateSampler(dev.device, &sci, nullptr, &sampler2), VK_SUCCESS,
                   "vkCreateSampler 2 failed");

    uint32_t id1 = khr_descriptor_heap_register_sampler(&heap, sampler1);
    TEST_ASSERT_EQ(id1, 0U, "First sampler must have index 0");
    TEST_ASSERT_EQ(heap.sampler_count, 1U, "Sampler count must be 1");

    /* Verify that descriptor bytes were directly written into host-mapped memory */
    const uint8_t* desc1 = (const uint8_t*)heap.sampler_host_map +
                           heap.sampler_binding_offset +
                           0 * heap.sampler_descriptor_size;
    bool nonzero1 = false;
    for (uint32_t b = 0; b < heap.sampler_descriptor_size; b++) {
        if (desc1[b] != 0) {
            nonzero1 = true;
            break;
        }
    }
    TEST_ASSERT(nonzero1, "vkGetDescriptorEXT must write non-zero descriptor data into host memory");

    uint32_t id2 = khr_descriptor_heap_register_sampler(&heap, sampler2);
    TEST_ASSERT_EQ(id2, 1U, "Second sampler must have index 1");
    TEST_ASSERT_EQ(heap.sampler_count, 2U, "Sampler count must be 2");

    const uint8_t* desc2 = (const uint8_t*)heap.sampler_host_map +
                           heap.sampler_binding_offset +
                           1 * heap.sampler_descriptor_size;
    bool nonzero2 = false;
    for (uint32_t b = 0; b < heap.sampler_descriptor_size; b++) {
        if (desc2[b] != 0) {
            nonzero2 = true;
            break;
        }
    }
    TEST_ASSERT(nonzero2, "Second sampler descriptor must have non-zero data");

    vkDestroySampler(dev.device, sampler1, nullptr);
    vkDestroySampler(dev.device, sampler2, nullptr);
    khr_descriptor_heap_destroy(&heap);
    khr_gfx_device_destroy(&dev);
    return true;
}

[[nodiscard]]
bool test_descriptor_heap_register_texture_direct_memory(void) {
    khr_gfx_device_t dev = {};
    if (!khr_gfx_device_init(&dev, (dev_t)0)) {
        return false;
    }

    khr_descriptor_heap_t heap = {};
    TEST_ASSERT(khr_descriptor_heap_init(&heap, &dev, 16, 4), "heap init failed");

    /* Create dummy 64x64 RGBA8 2D image and view */
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { .width = 64, .height = 64, .depth = 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage img1 = VK_NULL_HANDLE;
    TEST_ASSERT_EQ(vkCreateImage(dev.device, &ici, nullptr, &img1), VK_SUCCESS,
                   "vkCreateImage failed");

    VkMemoryRequirements mem_reqs = {};
    vkGetImageMemoryRequirements(dev.device, img1, &mem_reqs);

    VkPhysicalDeviceMemoryProperties mem_props = {};
    vkGetPhysicalDeviceMemoryProperties(dev.phy, &mem_props);
    uint32_t mem_type = UINT32_MAX;
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((mem_reqs.memoryTypeBits & (1U << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            mem_type = i;
            break;
        }
    }
    TEST_ASSERT(mem_type != UINT32_MAX, "Found device local memory type for image");

    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_reqs.size,
        .memoryTypeIndex = mem_type,
    };
    VkDeviceMemory mem1 = VK_NULL_HANDLE;
    TEST_ASSERT_EQ(vkAllocateMemory(dev.device, &ai, nullptr, &mem1), VK_SUCCESS,
                   "vkAllocateMemory for image failed");
    TEST_ASSERT_EQ(vkBindImageMemory(dev.device, img1, mem1, 0), VK_SUCCESS,
                   "vkBindImageMemory failed");

    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = img1,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    VkImageView view1 = VK_NULL_HANDLE;
    TEST_ASSERT_EQ(vkCreateImageView(dev.device, &vci, nullptr, &view1), VK_SUCCESS,
                   "vkCreateImageView failed");

    uint32_t tex_id = khr_descriptor_heap_register_texture(&heap, view1,
                                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    TEST_ASSERT_EQ(tex_id, 0U, "First registered texture must have ID 0");
    TEST_ASSERT_EQ(heap.texture_count, 1U, "Texture count must be 1");

    /* Verify that descriptor bytes were directly written into host-mapped memory */
    const uint8_t* desc = (const uint8_t*)heap.resource_host_map +
                          heap.resource_binding_offset +
                          0 * heap.resource_descriptor_size;
    bool nonzero = false;
    for (uint32_t b = 0; b < heap.resource_descriptor_size; b++) {
        if (desc[b] != 0) {
            nonzero = true;
            break;
        }
    }
    TEST_ASSERT(nonzero, "vkGetDescriptorEXT must write non-zero sampled-image descriptor into host memory");

    vkDestroyImageView(dev.device, view1, nullptr);
    vkDestroyImage(dev.device, img1, nullptr);
    vkFreeMemory(dev.device, mem1, nullptr);
    khr_descriptor_heap_destroy(&heap);
    khr_gfx_device_destroy(&dev);
    return true;
}

[[nodiscard]]
bool test_descriptor_heap_command_binding(void) {
    khr_gfx_device_t dev = {};
    if (!khr_gfx_device_init(&dev, (dev_t)0)) {
        return false;
    }

    khr_descriptor_heap_t heap = {};
    TEST_ASSERT(khr_descriptor_heap_init(&heap, &dev, 16, 4), "heap init failed");

    /* Create pipeline layout matching the descriptor buffer set layouts */
    VkDescriptorSetLayout set_layouts[2] = {
        heap.resource_set_layout,
        heap.sampler_set_layout,
    };
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 2,
        .pSetLayouts = set_layouts,
    };
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    TEST_ASSERT_EQ(vkCreatePipelineLayout(dev.device, &plci, nullptr, &pipeline_layout),
                   VK_SUCCESS, "vkCreatePipelineLayout failed");

    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = dev.cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    TEST_ASSERT_EQ(vkAllocateCommandBuffers(dev.device, &cbai, &cmd), VK_SUCCESS,
                   "vkAllocateCommandBuffers failed");

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    TEST_ASSERT_EQ(vkBeginCommandBuffer(cmd, &begin_info), VK_SUCCESS,
                   "vkBeginCommandBuffer failed");

    /* Test bind without errors */
    khr_descriptor_heap_bind(&heap, cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout);

    TEST_ASSERT_EQ(vkEndCommandBuffer(cmd), VK_SUCCESS, "vkEndCommandBuffer failed");

    vkFreeCommandBuffers(dev.device, dev.cmd_pool, 1, &cmd);
    vkDestroyPipelineLayout(dev.device, pipeline_layout, nullptr);
    khr_descriptor_heap_destroy(&heap);
    khr_gfx_device_destroy(&dev);
    return true;
}

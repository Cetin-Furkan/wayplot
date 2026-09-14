#include "test_framework.h"
#include "khoros/gfx/device.h"
#include "khoros/gfx/bda_arena.h"
#include "khoros/gfx/descriptor_buffer.h"
#include "khoros/gfx/texture.h"
#include "khoros/gfx/texture_synth.h"
#include <string.h>
#include <vulkan/vulkan.h>

/* Minimal DDS header layout for synthetic contract testing */
#pragma pack(push, 1)
typedef struct {
    uint32_t dwMagic;
    uint32_t dwSize;
    uint32_t dwFlags;
    uint32_t dwHeight;
    uint32_t dwWidth;
    uint32_t dwPitchOrLinearSize;
    uint32_t dwDepth;
    uint32_t dwMipMapCount;
    uint32_t dwReserved1[11];
    /* DDPIXELFORMAT */
    uint32_t pfSize;
    uint32_t pfFlags;
    uint32_t pfFourCC;
    uint32_t pfRGBBitCount;
    uint32_t pfRBitMask;
    uint32_t pfGBitMask;
    uint32_t pfBBitMask;
    uint32_t pfABitMask;
    /* Caps */
    uint32_t dwCaps;
    uint32_t dwCaps2;
    uint32_t dwCaps3;
    uint32_t dwCaps4;
    uint32_t dwReserved2;
    /* DDS_HEADER_DXT10 */
    uint32_t dxgiFormat;
    uint32_t resourceDimension;
    uint32_t miscFlag;
    uint32_t arraySize;
    uint32_t miscFlags2;
} test_dds_dx10_header_t;
#pragma pack(pop)

[[nodiscard]]
bool test_texture_header_parser_dds_dx10(void) {
    test_dds_dx10_header_t hdr = {
        .dwMagic = KHR_DDS_MAGIC,
        .dwSize = 124,
        .dwFlags = 0x1 | 0x2 | 0x4 | 0x8 | 0x20000, /* CAPS|HEIGHT|WIDTH|PIXELFORMAT|MIPMAPCOUNT */
        .dwHeight = 256,
        .dwWidth = 256,
        .dwDepth = 1,
        .dwMipMapCount = 3,
        .pfSize = 32,
        .pfFlags = 0x4, /* DDPF_FOURCC */
        .pfFourCC = KHR_DDS_FOURCC_DX10,
        .dxgiFormat = 99, /* DXGI_FORMAT_BC7_UNORM_SRGB */
        .resourceDimension = 3, /* D3D10_RESOURCE_DIMENSION_TEXTURE2D */
        .arraySize = 1,
    };

    uint8_t buffer[sizeof(hdr) + 128 * 1024] = {};
    memcpy(buffer, &hdr, sizeof(hdr));

    khr_texture_info_t info = {};
    TEST_ASSERT(khr_texture_parse_header(buffer, sizeof(buffer), &info), "parse DDS DX10 header");
    TEST_ASSERT_EQ(info.width, 256U, "width must be 256");
    TEST_ASSERT_EQ(info.height, 256U, "height must be 256");
    TEST_ASSERT_EQ(info.mip_levels, 3U, "mip count must be 3");
    TEST_ASSERT_EQ((int)info.format, (int)VK_FORMAT_BC7_SRGB_BLOCK, "BC7 format mapping");
    TEST_ASSERT(info.is_block_compressed, "is block compressed");
    TEST_ASSERT_EQ(info.block_bytes, 16U, "BC7 block size is 16 bytes");

    /* Mip 0: 256x256 -> 64x64 blocks * 16 = 65,536 bytes */
    TEST_ASSERT_EQ(info.mips[0].width, 256U, "mip 0 width");
    TEST_ASSERT_EQ(info.mips[0].height, 256U, "mip 0 height");
    TEST_ASSERT_EQ(info.mips[0].byte_size, 65536U, "mip 0 size");
    TEST_ASSERT_EQ(info.mips[0].byte_offset, 0U, "mip 0 offset");

    /* Mip 1: 128x128 -> 32x32 blocks * 16 = 16,384 bytes */
    TEST_ASSERT_EQ(info.mips[1].width, 128U, "mip 1 width");
    TEST_ASSERT_EQ(info.mips[1].byte_size, 16384U, "mip 1 size");
    TEST_ASSERT_EQ(info.mips[1].byte_offset, 65536U, "mip 1 offset");

    /* Mip 2: 64x64 -> 16x16 blocks * 16 = 4,096 bytes */
    TEST_ASSERT_EQ(info.mips[2].width, 64U, "mip 2 width");
    TEST_ASSERT_EQ(info.mips[2].byte_size, 4096U, "mip 2 size");
    TEST_ASSERT_EQ(info.mips[2].byte_offset, 65536U + 16384U, "mip 2 offset");

    /* Test Corrupt Buffer Rejection */
    TEST_ASSERT(!khr_texture_parse_header(buffer, 32, &info), "Reject truncated buffer");
    buffer[0] = 0x00;
    TEST_ASSERT(!khr_texture_parse_header(buffer, sizeof(buffer), &info), "Reject invalid magic");

    return true;
}

[[nodiscard]]
bool test_texture_header_parser_ktex(void) {
    uint8_t buffer[64 + 32 * 1024] = {};
    uint32_t* u = (uint32_t*)(void*)buffer;
    u[0] = KHR_TEX_MAGIC;
    u[1] = KHR_TEX_VERSION;
    u[2] = (uint32_t)VK_FORMAT_BC5_UNORM_BLOCK;
    u[3] = 128; /* width */
    u[4] = 128; /* height */
    u[5] = 1;   /* depth */
    u[6] = 2;   /* mips */
    u[7] = 1;   /* layers */
    u[8] = 64;  /* payload offset */
    u[9] = 16384 + 4096; /* total payload size */

    khr_texture_info_t info = {};
    TEST_ASSERT(khr_texture_parse_header(buffer, sizeof(buffer), &info), "parse KTEX header");
    TEST_ASSERT_EQ(info.width, 128U, "ktex width");
    TEST_ASSERT_EQ(info.height, 128U, "ktex height");
    TEST_ASSERT_EQ(info.mip_levels, 2U, "ktex mips");
    TEST_ASSERT_EQ((int)info.format, (int)VK_FORMAT_BC5_UNORM_BLOCK, "ktex format BC5");
    TEST_ASSERT(info.is_block_compressed, "BC5 is block compressed");
    TEST_ASSERT_EQ(info.block_bytes, 16U, "BC5 block size is 16 bytes");
    TEST_ASSERT_EQ(info.header_size, 64U, "payload offset is 64");

    return true;
}

[[nodiscard]]
bool test_texture_optimal_image_allocation(void) {
    khr_gfx_device_t dev = {};
    TEST_ASSERT(khr_gfx_device_init(&dev, (dev_t)0), "gfx device init");

    khr_texture_t tex = {};
    bool ok = khr_texture_create_2d(&tex, &dev, 128, 128, VK_FORMAT_R8G8B8A8_UNORM, 1,
                                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
    TEST_ASSERT(ok, "khr_texture_create_2d RGBA8");
    TEST_ASSERT_NOT_NULL(tex.image, "texture image valid");
    TEST_ASSERT_NOT_NULL(tex.memory, "texture memory valid");
    TEST_ASSERT_NOT_NULL(tex.view, "texture view valid");
    TEST_ASSERT_EQ(tex.width, 128U, "texture width 128");
    TEST_ASSERT_EQ(tex.height, 128U, "texture height 128");
    TEST_ASSERT_EQ(tex.descriptor_index, UINT32_MAX, "initial descriptor index UINT32_MAX");

    khr_texture_destroy(&tex);
    TEST_ASSERT_NULL(tex.image, "texture image null after destroy");
    TEST_ASSERT_NULL(tex.view, "texture view null after destroy");

    khr_gfx_device_destroy(&dev);
    return true;
}

[[nodiscard]]
bool test_texture_descriptor_buffer_registration(void) {
    khr_gfx_device_t dev = {};
    TEST_ASSERT(khr_gfx_device_init(&dev, (dev_t)0), "gfx device init");

    khr_descriptor_heap_t heap = {};
    TEST_ASSERT(khr_descriptor_heap_init(&heap, &dev, 64, 16), "descriptor heap init");

    khr_texture_t tex = {};
    TEST_ASSERT(khr_texture_create_2d(&tex, &dev, 64, 64, VK_FORMAT_R8G8B8A8_UNORM, 1,
                                      VK_IMAGE_USAGE_SAMPLED_BIT), "create tex");

    TEST_ASSERT(khr_texture_register_heap(&tex, &heap), "register in descriptor heap");
    TEST_ASSERT_EQ(tex.descriptor_index, 0U, "first texture assigned index 0");
    TEST_ASSERT_EQ(heap.texture_count, 1U, "heap texture count 1");

    /* Register second texture */
    khr_texture_t tex2 = {};
    TEST_ASSERT(khr_texture_create_2d(&tex2, &dev, 64, 64, VK_FORMAT_R8G8B8A8_UNORM, 1,
                                      VK_IMAGE_USAGE_SAMPLED_BIT), "create tex2");
    TEST_ASSERT(khr_texture_register_heap(&tex2, &heap), "register tex2");
    TEST_ASSERT_EQ(tex2.descriptor_index, 1U, "second texture assigned index 1");
    TEST_ASSERT_EQ(heap.texture_count, 2U, "heap texture count 2");

    khr_texture_destroy(&tex2);
    khr_texture_destroy(&tex);
    khr_descriptor_heap_destroy(&heap);
    khr_gfx_device_destroy(&dev);
    return true;
}

[[nodiscard]]
bool test_texture_gpu_compute_synthesizer(void) {
    khr_gfx_device_t dev = {};
    TEST_ASSERT(khr_gfx_device_init(&dev, (dev_t)0), "gfx device init");

    khr_texture_synth_pipeline_t synth = {};
    TEST_ASSERT(khr_texture_synth_pipeline_init(&synth, &dev), "synth pipeline init");
    TEST_ASSERT_NOT_NULL(synth.pipeline, "pipeline valid");
    TEST_ASSERT_NOT_NULL(synth.desc_buffer, "desc buffer valid");

    khr_descriptor_heap_t heap = {};
    TEST_ASSERT(khr_descriptor_heap_init(&heap, &dev, 16, 4), "heap init");

    /* 1. Generate Damascus Steel Pattern */
    khr_texture_t tex_damascus = {};
    float steel_tint[4] = { 0.9f, 0.95f, 1.0f, 1.0f };
    TEST_ASSERT(khr_texture_synth_generate_2d(&synth, &tex_damascus, 128, 128,
                                             KHR_TEX_SYNTH_DAMASCUS_STEEL, 10.0f, steel_tint),
                "generate damascus steel texture");
    TEST_ASSERT(khr_texture_register_heap(&tex_damascus, &heap), "register damascus in heap");
    TEST_ASSERT_EQ(tex_damascus.descriptor_index, 0U, "damascus tex id 0");

    /* 2. Generate Tangent-Space Normal Map */
    khr_texture_t tex_normal = {};
    TEST_ASSERT(khr_texture_synth_generate_2d(&synth, &tex_normal, 128, 128,
                                             KHR_TEX_SYNTH_NORMAL_MAP, 12.0f, nullptr),
                "generate normal map texture");
    TEST_ASSERT(khr_texture_register_heap(&tex_normal, &heap), "register normal in heap");
    TEST_ASSERT_EQ(tex_normal.descriptor_index, 1U, "normal tex id 1");

    /* 3. Generate Brushed Metal Pattern */
    khr_texture_t tex_brushed = {};
    float gold_tint[4] = { 1.0f, 0.8f, 0.4f, 1.0f };
    TEST_ASSERT(khr_texture_synth_generate_2d(&synth, &tex_brushed, 128, 128,
                                             KHR_TEX_SYNTH_BRUSHED_METAL, 8.0f, gold_tint),
                "generate brushed metal texture");
    TEST_ASSERT(khr_texture_register_heap(&tex_brushed, &heap), "register brushed in heap");
    TEST_ASSERT_EQ(tex_brushed.descriptor_index, 2U, "brushed tex id 2");

    /* 4. Generate PBR Calibration Checker */
    khr_texture_t tex_checker = {};
    float checker_tint[4] = { 0.8f, 0.2f, 0.2f, 1.0f };
    TEST_ASSERT(khr_texture_synth_generate_2d(&synth, &tex_checker, 128, 128,
                                             KHR_TEX_SYNTH_PBR_CHECKER, 4.0f, checker_tint),
                "generate checker texture");
    TEST_ASSERT(khr_texture_register_heap(&tex_checker, &heap), "register checker in heap");
    TEST_ASSERT_EQ(tex_checker.descriptor_index, 3U, "checker tex id 3");

    /* Cleanup */
    khr_texture_destroy(&tex_checker);
    khr_texture_destroy(&tex_brushed);
    khr_texture_destroy(&tex_normal);
    khr_texture_destroy(&tex_damascus);
    khr_descriptor_heap_destroy(&heap);
    khr_texture_synth_pipeline_destroy(&synth);
    khr_gfx_device_destroy(&dev);
    return true;
}

[[nodiscard]]
bool test_texture_gpu_copy2_staging_upload(void) {
    khr_gfx_device_t dev = {};
    TEST_ASSERT(khr_gfx_device_init(&dev, (dev_t)0), "gfx device init");

    khr_bda_arena_t arena = {};
    TEST_ASSERT(khr_bda_arena_init(&dev, &arena, 256 * 1024), "arena init");

    /* Allocate staging memory */
    constexpr uint32_t TEX_W = 64;
    constexpr uint32_t TEX_H = 64;
    constexpr uint32_t PAYLOAD_SZ = TEX_W * TEX_H * 4;
    void* stage_host = nullptr;
    VkDeviceAddress stage_gpu = 0;
    TEST_ASSERT(khr_bda_arena_alloc(&arena, PAYLOAD_SZ, 64, &stage_host, &stage_gpu), "alloc staging");

    /* Fill staging with synthetic pattern */
    uint32_t* pixels = (uint32_t*)stage_host;
    for (uint32_t i = 0; i < TEX_W * TEX_H; i++) {
        pixels[i] = 0xFF00FF00U; /* Solid green RGBA */
    }

    /* Create destination GPU texture */
    khr_texture_t tex = {};
    TEST_ASSERT(khr_texture_create_2d(&tex, &dev, TEX_W, TEX_H, VK_FORMAT_R8G8B8A8_UNORM, 1,
                                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT),
                "create dest texture");

    khr_texture_info_t info = {
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .width = TEX_W,
        .height = TEX_H,
        .depth = 1,
        .mip_levels = 1,
        .layer_count = 1,
        .total_bytes = PAYLOAD_SZ,
        .is_block_compressed = false,
        .block_width = 1,
        .block_height = 1,
        .block_bytes = 4,
        .header_size = 0,
        .mips[0] = {
            .width = TEX_W,
            .height = TEX_H,
            .depth = 1,
            .byte_offset = 0,
            .byte_size = PAYLOAD_SZ,
            .row_pitch = TEX_W * 4,
        },
    };

    /* Allocate and record command buffer */
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = dev.cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    TEST_ASSERT_EQ(vkAllocateCommandBuffers(dev.device, &cbai, &cmd), VK_SUCCESS, "alloc cmd");

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    TEST_ASSERT_EQ(vkBeginCommandBuffer(cmd, &begin_info), VK_SUCCESS, "begin cmd");

    VkDeviceSize stage_base_offset = (VkDeviceSize)((uintptr_t)stage_host - (uintptr_t)arena.host_ptr);
    khr_texture_record_upload(cmd, &tex, arena.buffer, stage_base_offset, &info);

    TEST_ASSERT_EQ(vkEndCommandBuffer(cmd), VK_SUCCESS, "end cmd");

    VkCommandBufferSubmitInfo cbsi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = cmd,
    };
    VkSubmitInfo2 si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &cbsi,
    };
    TEST_ASSERT_EQ(vkQueueSubmit2(dev.gfx_queue, 1, &si, VK_NULL_HANDLE), VK_SUCCESS, "submit2 upload");
    TEST_ASSERT_EQ(vkQueueWaitIdle(dev.gfx_queue), VK_SUCCESS, "wait idle upload");

    TEST_ASSERT_EQ((int)tex.current_layout, (int)VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   "texture transitioned to SHADER_READ_ONLY_OPTIMAL");

    vkFreeCommandBuffers(dev.device, dev.cmd_pool, 1, &cmd);
    khr_texture_destroy(&tex);
    khr_bda_arena_destroy(&dev, &arena);
    khr_gfx_device_destroy(&dev);
    return true;
}

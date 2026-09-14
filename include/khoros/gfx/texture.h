#ifndef KHOROS_GFX_TEXTURE_H
#define KHOROS_GFX_TEXTURE_H

#if __STDC_VERSION__ < 202311L
#error "Khoros requires pure ISO C23 (compile with -std=c23)"
#endif

#ifdef _STDBOOL_H
#error "<stdbool.h> is strictly banned. In C23, bool, true, and false are native language keywords."
#endif

#ifdef _STDALIGN_H
#error "<stdalign.h> is strictly banned. In C23, alignas and alignof are native language keywords."
#endif

#include "khoros/core/attributes.h"
#include "khoros/gfx/device.h"
#include "khoros/gfx/descriptor_buffer.h"
#include <stdint.h>
#include <vulkan/vulkan.h>

constexpr uint32_t KHR_TEX_MAX_MIPS = 16;
constexpr uint32_t KHR_TEX_MAGIC = 0x5445584BU; /* 'KTEX' */
constexpr uint32_t KHR_TEX_VERSION = 1U;

constexpr uint32_t KHR_DDS_MAGIC = 0x20534444U; /* 'DDS ' */
constexpr uint32_t KHR_DDS_FOURCC_DX10 = 0x30315844U; /* 'DX10' */

/*
 * Mip level layout information for direct zero-copy GPU transfers.
 */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t byte_offset; /* Offset from start of texture payload */
    uint32_t byte_size;   /* Size in bytes of this mip level */
    uint32_t row_pitch;   /* Row pitch in bytes (for block rows or pixel rows) */
} khr_texture_mip_t;

/*
 * Parsed header metadata from a GPU-native texture container (DDS / KHR_TEX).
 * All dimensions, mip chains, and formats are extracted with zero CPU pixel decoding.
 */
typedef struct {
    VkFormat          format;
    uint32_t          width;
    uint32_t          height;
    uint32_t          depth;
    uint32_t          mip_levels;
    uint32_t          layer_count;
    uint32_t          total_bytes;
    bool              is_block_compressed;
    uint32_t          block_width;
    uint32_t          block_height;
    uint32_t          block_bytes;
    size_t            header_size; /* Byte offset where raw image payload starts */
    khr_texture_mip_t mips[KHR_TEX_MAX_MIPS];
} khr_texture_info_t;

/*
 * Modern GPU Texture Representation (Pillar 1).
 * Optimal device-local tiled memory, paired with a modern Vulkan 1.4 image view
 * and registered directly into a VK_EXT_descriptor_buffer heap.
 */
typedef struct {
    const khr_gfx_device_t* dev;
    VkImage                 image;
    VkDeviceMemory          memory;
    VkImageView             view;
    VkFormat                format;
    uint32_t                width;
    uint32_t                height;
    uint32_t                depth;
    uint32_t                mip_levels;
    uint32_t                layer_count;
    VkImageLayout           current_layout;
    uint32_t                descriptor_index; /* Index in khr_descriptor_heap_t */
    bool                    initialized;
} khr_texture_t;

/*
 * Parse header metadata from raw DDS or KHR_TEX buffer in memory.
 * Extracts format, dimensions, and mip offsets in O(1) arithmetic with zero CPU pixel processing.
 */
[[nodiscard]]
bool khr_texture_parse_header(const void* data, size_t size, khr_texture_info_t* out_info);

/*
 * Create optimal device-local 2D texture (Vulkan 1.4 core).
 * Allocates optimal tiled device memory and creates standard VkImageView.
 */
[[nodiscard]]
bool khr_texture_create_2d(khr_texture_t* tex,
                           const khr_gfx_device_t* dev,
                           uint32_t width,
                           uint32_t height,
                           VkFormat format,
                           uint32_t mip_levels,
                           VkImageUsageFlags usage);

/*
 * Destroy GPU texture, releasing image, memory, and view.
 */
void khr_texture_destroy(khr_texture_t* tex);

/*
 * Record pure GPU upload commands from host-coherent BDA staging buffer to optimal VRAM.
 * Uses vkCmdPipelineBarrier2 and vkCmdCopyBufferToImage2 (Vulkan 1.3/1.4 core).
 * Automatically handles UNDEFINED -> TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL transitions.
 */
void khr_texture_record_upload(VkCommandBuffer cmd,
                               khr_texture_t* tex,
                               VkBuffer staging_buffer,
                               VkDeviceSize staging_base_offset,
                               const khr_texture_info_t* info);

/*
 * Register texture into a unified descriptor heap (VK_EXT_descriptor_buffer).
 * Directly writes sampled image descriptor into host-coherent GPU descriptor memory.
 * Records the allocated index into tex->descriptor_index.
 */
[[nodiscard]]
bool khr_texture_register_heap(khr_texture_t* tex, khr_descriptor_heap_t* heap);

#endif /* KHOROS_GFX_TEXTURE_H */

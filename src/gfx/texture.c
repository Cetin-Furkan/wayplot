#include "khoros/gfx/texture.h"
#include <string.h>

/* DirectDraw Surface (DDS) format structure headers (MSDN / DXGI specification) */
typedef struct {
    uint32_t size;
    uint32_t flags;
    uint32_t four_cc;
    uint32_t rgb_bit_count;
    uint32_t r_bit_mask;
    uint32_t g_bit_mask;
    uint32_t b_bit_mask;
    uint32_t a_bit_mask;
} dds_pixel_format_t;

typedef struct {
    uint32_t           size;
    uint32_t           flags;
    uint32_t           height;
    uint32_t           width;
    uint32_t           pitch_or_linear_size;
    uint32_t           depth;
    uint32_t           mip_map_count;
    uint32_t           reserved1[11];
    dds_pixel_format_t ddspf;
    uint32_t           caps;
    uint32_t           caps2;
    uint32_t           caps3;
    uint32_t           caps4;
    uint32_t           reserved2;
} dds_header_t;

typedef struct {
    uint32_t dxgi_format;
    uint32_t resource_dimension;
    uint32_t misc_flag;
    uint32_t array_size;
    uint32_t misc_flags2;
} dds_header_dx10_t;

/* Khoros Native GPU Texture Header */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t format; /* VkFormat */
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t mip_levels;
    uint32_t layer_count;
    uint32_t total_payload_bytes;
    uint32_t reserved[7];
} khr_tex_header_t;

[[nodiscard]]
static uint32_t khr_texture_find_device_local_mem(VkPhysicalDevice phy, uint32_t type_filter) {
    VkPhysicalDeviceMemoryProperties mem_props = {};
    vkGetPhysicalDeviceMemoryProperties(phy, &mem_props);

    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((type_filter & (1U << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            return i;
        }
    }
    return UINT32_MAX;
}

static void khr_texture_compute_mip_chain(khr_texture_info_t* info) {
    uint32_t cur_w = info->width;
    uint32_t cur_h = info->height;
    uint32_t cur_d = info->depth;
    uint32_t cur_offset = 0;

    for (uint32_t i = 0; i < info->mip_levels; i++) {
        info->mips[i].width = cur_w;
        info->mips[i].height = cur_h;
        info->mips[i].depth = cur_d;
        info->mips[i].byte_offset = cur_offset;

        uint32_t level_bytes = 0;
        uint32_t row_pitch = 0;
        if (info->is_block_compressed) {
            uint32_t bw = (cur_w + info->block_width - 1U) / info->block_width;
            uint32_t bh = (cur_h + info->block_height - 1U) / info->block_height;
            if (bw == 0) bw = 1;
            if (bh == 0) bh = 1;
            row_pitch = bw * info->block_bytes;
            level_bytes = row_pitch * bh * cur_d;
        } else {
            row_pitch = cur_w * info->block_bytes;
            level_bytes = row_pitch * cur_h * cur_d;
        }

        info->mips[i].row_pitch = row_pitch;
        info->mips[i].byte_size = level_bytes;

        cur_offset += level_bytes;
        cur_w = (cur_w > 1U) ? (cur_w >> 1U) : 1U;
        cur_h = (cur_h > 1U) ? (cur_h >> 1U) : 1U;
        cur_d = (cur_d > 1U) ? (cur_d >> 1U) : 1U;
    }
    info->total_bytes = cur_offset;
}

bool khr_texture_parse_header(const void* data, size_t size, khr_texture_info_t* out_info) {
    if (data == nullptr || size < sizeof(uint32_t) || out_info == nullptr) {
        return false;
    }
    memset(out_info, 0, sizeof(*out_info));

    const uint8_t* bytes = (const uint8_t*)data;
    uint32_t magic = *(const uint32_t*)bytes;

    /* 1. Native Khoros GPU Texture Container */
    if (magic == KHR_TEX_MAGIC) {
        if (size < sizeof(khr_tex_header_t)) {
            return false;
        }
        const khr_tex_header_t* hdr = (const khr_tex_header_t*)bytes;
        if (hdr->version != KHR_TEX_VERSION || hdr->width == 0 || hdr->height == 0) {
            return false;
        }

        out_info->format = (VkFormat)hdr->format;
        out_info->width = hdr->width;
        out_info->height = hdr->height;
        out_info->depth = (hdr->depth > 0) ? hdr->depth : 1U;
        out_info->mip_levels = (hdr->mip_levels > 0 && hdr->mip_levels <= KHR_TEX_MAX_MIPS) ? hdr->mip_levels : 1U;
        out_info->layer_count = (hdr->layer_count > 0) ? hdr->layer_count : 1U;
        out_info->header_size = sizeof(khr_tex_header_t);

        /* Determine block compression properties */
        switch (out_info->format) {
            case VK_FORMAT_BC7_UNORM_BLOCK:
            case VK_FORMAT_BC7_SRGB_BLOCK:
                out_info->is_block_compressed = true;
                out_info->block_width = 4;
                out_info->block_height = 4;
                out_info->block_bytes = 16;
                break;
            case VK_FORMAT_BC5_UNORM_BLOCK:
            case VK_FORMAT_BC5_SNORM_BLOCK:
                out_info->is_block_compressed = true;
                out_info->block_width = 4;
                out_info->block_height = 4;
                out_info->block_bytes = 16;
                break;
            case VK_FORMAT_BC4_UNORM_BLOCK:
            case VK_FORMAT_BC4_SNORM_BLOCK:
                out_info->is_block_compressed = true;
                out_info->block_width = 4;
                out_info->block_height = 4;
                out_info->block_bytes = 8;
                break;
            case VK_FORMAT_R8G8B8A8_UNORM:
            case VK_FORMAT_R8G8B8A8_SRGB:
                out_info->is_block_compressed = false;
                out_info->block_width = 1;
                out_info->block_height = 1;
                out_info->block_bytes = 4;
                break;
            case VK_FORMAT_R16G16B16A16_SFLOAT:
                out_info->is_block_compressed = false;
                out_info->block_width = 1;
                out_info->block_height = 1;
                out_info->block_bytes = 8;
                break;
            default:
                return false;
        }

        khr_texture_compute_mip_chain(out_info);
        return true;
    }

    /* 2. DirectDraw Surface (DDS) Container */
    if (magic == KHR_DDS_MAGIC) {
        if (size < sizeof(uint32_t) + sizeof(dds_header_t)) {
            return false;
        }
        const dds_header_t* dds = (const dds_header_t*)(bytes + sizeof(uint32_t));
        if (dds->size != sizeof(dds_header_t) || dds->width == 0 || dds->height == 0) {
            return false;
        }

        out_info->width = dds->width;
        out_info->height = dds->height;
        out_info->depth = (dds->depth > 0) ? dds->depth : 1U;
        out_info->mip_levels = (dds->mip_map_count > 0 && dds->mip_map_count <= KHR_TEX_MAX_MIPS) ? dds->mip_map_count : 1U;
        out_info->layer_count = 1;
        out_info->header_size = sizeof(uint32_t) + sizeof(dds_header_t);

        /* Check for DX10 extended header */
        if ((dds->ddspf.flags & 0x00000004U) && dds->ddspf.four_cc == KHR_DDS_FOURCC_DX10) {
            if (size < out_info->header_size + sizeof(dds_header_dx10_t)) {
                return false;
            }
            const dds_header_dx10_t* dx10 = (const dds_header_dx10_t*)(bytes + out_info->header_size);
            out_info->header_size += sizeof(dds_header_dx10_t);
            if (dx10->array_size > 1) {
                out_info->layer_count = dx10->array_size;
            }

            switch (dx10->dxgi_format) {
                case 99: /* DXGI_FORMAT_BC7_UNORM_SRGB */
                    out_info->format = VK_FORMAT_BC7_SRGB_BLOCK;
                    out_info->is_block_compressed = true;
                    out_info->block_width = 4;
                    out_info->block_height = 4;
                    out_info->block_bytes = 16;
                    break;
                case 98: /* DXGI_FORMAT_BC7_UNORM */
                    out_info->format = VK_FORMAT_BC7_UNORM_BLOCK;
                    out_info->is_block_compressed = true;
                    out_info->block_width = 4;
                    out_info->block_height = 4;
                    out_info->block_bytes = 16;
                    break;
                case 83: /* DXGI_FORMAT_BC5_UNORM */
                    out_info->format = VK_FORMAT_BC5_UNORM_BLOCK;
                    out_info->is_block_compressed = true;
                    out_info->block_width = 4;
                    out_info->block_height = 4;
                    out_info->block_bytes = 16;
                    break;
                case 80: /* DXGI_FORMAT_BC4_UNORM */
                    out_info->format = VK_FORMAT_BC4_UNORM_BLOCK;
                    out_info->is_block_compressed = true;
                    out_info->block_width = 4;
                    out_info->block_height = 4;
                    out_info->block_bytes = 8;
                    break;
                case 29: /* DXGI_FORMAT_R8G8B8A8_UNORM_SRGB */
                    out_info->format = VK_FORMAT_R8G8B8A8_SRGB;
                    out_info->is_block_compressed = false;
                    out_info->block_width = 1;
                    out_info->block_height = 1;
                    out_info->block_bytes = 4;
                    break;
                case 28: /* DXGI_FORMAT_R8G8B8A8_UNORM */
                    out_info->format = VK_FORMAT_R8G8B8A8_UNORM;
                    out_info->is_block_compressed = false;
                    out_info->block_width = 1;
                    out_info->block_height = 1;
                    out_info->block_bytes = 4;
                    break;
                default:
                    return false;
            }
        } else {
            /* Legacy FourCC lookup */
            uint32_t fcc = dds->ddspf.four_cc;
            if (fcc == 0x31545844) { /* 'DXT1' */
                out_info->format = VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
                out_info->is_block_compressed = true;
                out_info->block_width = 4;
                out_info->block_height = 4;
                out_info->block_bytes = 8;
            } else if (fcc == 0x35545844) { /* 'DXT5' */
                out_info->format = VK_FORMAT_BC3_SRGB_BLOCK;
                out_info->is_block_compressed = true;
                out_info->block_width = 4;
                out_info->block_height = 4;
                out_info->block_bytes = 16;
            } else if (fcc == 0x32495441 || fcc == 0x55354342) { /* 'ATI2' or 'BC5U' */
                out_info->format = VK_FORMAT_BC5_UNORM_BLOCK;
                out_info->is_block_compressed = true;
                out_info->block_width = 4;
                out_info->block_height = 4;
                out_info->block_bytes = 16;
            } else if ((dds->ddspf.flags & 0x00000040U) && dds->ddspf.rgb_bit_count == 32) {
                out_info->format = VK_FORMAT_R8G8B8A8_UNORM;
                out_info->is_block_compressed = false;
                out_info->block_width = 1;
                out_info->block_height = 1;
                out_info->block_bytes = 4;
            } else {
                return false;
            }
        }

        khr_texture_compute_mip_chain(out_info);
        return true;
    }

    return false;
}

bool khr_texture_create_2d(khr_texture_t* tex,
                           const khr_gfx_device_t* dev,
                           uint32_t width,
                           uint32_t height,
                           VkFormat format,
                           uint32_t mip_levels,
                           VkImageUsageFlags usage) {
    if (tex == nullptr || dev == nullptr || dev->device == VK_NULL_HANDLE ||
        width == 0 || height == 0) {
        return false;
    }
    memset(tex, 0, sizeof(*tex));
    tex->dev = dev;
    tex->format = format;
    tex->width = width;
    tex->height = height;
    tex->depth = 1;
    tex->mip_levels = (mip_levels > 0) ? mip_levels : 1U;
    tex->layer_count = 1;
    tex->current_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    tex->descriptor_index = UINT32_MAX;

    /* 1. Create Device-Local Optimal Tiled Image */
    VkImageCreateInfo ici = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags         = 0,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = format,
        .extent        = { .width = width, .height = height, .depth = 1 },
        .mipLevels     = tex->mip_levels,
        .arrayLayers   = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = usage | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(dev->device, &ici, nullptr, &tex->image) != VK_SUCCESS) {
        return false;
    }

    /* 2. Query Memory Requirements and Allocate in DEVICE_LOCAL VRAM */
    VkMemoryRequirements mem_reqs = {};
    vkGetImageMemoryRequirements(dev->device, tex->image, &mem_reqs);

    uint32_t mem_type = khr_texture_find_device_local_mem(dev->phy, mem_reqs.memoryTypeBits);
    if (mem_type == UINT32_MAX) {
        vkDestroyImage(dev->device, tex->image, nullptr);
        tex->image = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo ai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = mem_reqs.size,
        .memoryTypeIndex = mem_type,
    };
    if (vkAllocateMemory(dev->device, &ai, nullptr, &tex->memory) != VK_SUCCESS) {
        vkDestroyImage(dev->device, tex->image, nullptr);
        tex->image = VK_NULL_HANDLE;
        return false;
    }
    if (vkBindImageMemory(dev->device, tex->image, tex->memory, 0) != VK_SUCCESS) {
        khr_texture_destroy(tex);
        return false;
    }

    /* 3. Create Default Shader Resource View */
    VkImageViewCreateInfo vci = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image    = tex->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = format,
        .components = {
            .r = VK_COMPONENT_SWIZZLE_IDENTITY,
            .g = VK_COMPONENT_SWIZZLE_IDENTITY,
            .b = VK_COMPONENT_SWIZZLE_IDENTITY,
            .a = VK_COMPONENT_SWIZZLE_IDENTITY,
        },
        .subresourceRange = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0,
            .levelCount     = tex->mip_levels,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
    };
    if (vkCreateImageView(dev->device, &vci, nullptr, &tex->view) != VK_SUCCESS) {
        khr_texture_destroy(tex);
        return false;
    }

    tex->initialized = true;
    return true;
}

void khr_texture_destroy(khr_texture_t* tex) {
    if (tex == nullptr || tex->dev == nullptr || tex->dev->device == VK_NULL_HANDLE) {
        return;
    }
    VkDevice d = tex->dev->device;
    if (tex->view != VK_NULL_HANDLE) {
        vkDestroyImageView(d, tex->view, nullptr);
        tex->view = VK_NULL_HANDLE;
    }
    if (tex->image != VK_NULL_HANDLE) {
        vkDestroyImage(d, tex->image, nullptr);
        tex->image = VK_NULL_HANDLE;
    }
    if (tex->memory != VK_NULL_HANDLE) {
        vkFreeMemory(d, tex->memory, nullptr);
        tex->memory = VK_NULL_HANDLE;
    }
    tex->initialized = false;
}

void khr_texture_record_upload(VkCommandBuffer cmd,
                               khr_texture_t* tex,
                               VkBuffer staging_buffer,
                               VkDeviceSize staging_base_offset,
                               const khr_texture_info_t* info) {
    if (cmd == VK_NULL_HANDLE || tex == nullptr || !tex->initialized ||
        staging_buffer == VK_NULL_HANDLE || info == nullptr) {
        return;
    }

    /* 1. Vulkan 1.4 Transition: UNDEFINED -> TRANSFER_DST_OPTIMAL */
    VkImageMemoryBarrier2 pre_barrier = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask        = VK_PIPELINE_STAGE_2_NONE,
        .srcAccessMask       = VK_ACCESS_2_NONE,
        .dstStageMask        = VK_PIPELINE_STAGE_2_COPY_BIT,
        .dstAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = tex->image,
        .subresourceRange    = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0,
            .levelCount     = tex->mip_levels,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
    };
    VkDependencyInfo pre_dep = {
        .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers    = &pre_barrier,
    };
    vkCmdPipelineBarrier2(cmd, &pre_dep);

    /* 2. Pure GPU DMA Copy: Staging Buffer -> Optimal Tiled VRAM */
    VkBufferImageCopy2 copies[KHR_TEX_MAX_MIPS] = {};
    uint32_t copy_count = (info->mip_levels <= KHR_TEX_MAX_MIPS) ? info->mip_levels : KHR_TEX_MAX_MIPS;
    for (uint32_t i = 0; i < copy_count; i++) {
        copies[i] = (VkBufferImageCopy2){
            .sType             = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
            .bufferOffset      = staging_base_offset + info->header_size + info->mips[i].byte_offset,
            .bufferRowLength   = 0,
            .bufferImageHeight = 0,
            .imageSubresource  = {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel       = i,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
            .imageOffset       = { 0, 0, 0 },
            .imageExtent       = {
                .width  = info->mips[i].width,
                .height = info->mips[i].height,
                .depth  = 1,
            },
        };
    }

    VkCopyBufferToImageInfo2 copy_info = {
        .sType          = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
        .srcBuffer      = staging_buffer,
        .dstImage       = tex->image,
        .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .regionCount    = copy_count,
        .pRegions       = copies,
    };
    vkCmdCopyBufferToImage2(cmd, &copy_info);

    /* 3. Vulkan 1.4 Transition: TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL */
    VkImageMemoryBarrier2 post_barrier = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask        = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask        = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask       = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = tex->image,
        .subresourceRange    = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0,
            .levelCount     = tex->mip_levels,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
    };
    VkDependencyInfo post_dep = {
        .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers    = &post_barrier,
    };
    vkCmdPipelineBarrier2(cmd, &post_dep);
    tex->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

bool khr_texture_register_heap(khr_texture_t* tex, khr_descriptor_heap_t* heap) {
    if (tex == nullptr || !tex->initialized || heap == nullptr || !heap->initialized) {
        return false;
    }
    uint32_t idx = khr_descriptor_heap_register_texture(heap, tex->view,
                                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (idx == UINT32_MAX) {
        return false;
    }
    tex->descriptor_index = idx;
    return true;
}

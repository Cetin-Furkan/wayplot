#ifndef KHOROS_GFX_HIZ_H
#define KHOROS_GFX_HIZ_H

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
#include "khoros/gfx/gpu_math.h"
#include <stdint.h>
#include <vulkan/vulkan.h>

constexpr uint32_t KHR_HIZ_MAX_MIP_LEVELS = 16;

/*
 * Push constants for two-pass Hi-Z compute culling (cull_hiz.slang).
 * Strictly 128 bytes (Vulkan hardware guarantee limit).
 */
typedef struct {
    alignas(8) VkDeviceAddress instances_addr;        /* BDA: const khr_gpu_instance_t* (offset 0) */
    VkDeviceAddress culled_instances_addr;            /* BDA: khr_gpu_culled_instance_t* (offset 8) */
    VkDeviceAddress draw_cmd_addr;                    /* BDA: khr_draw_indirect_cmd_t* (offset 16) */
    VkDeviceAddress draw_count_addr;                  /* BDA: uint32_t* atomic count (offset 24) */
    uint32_t        instance_count;                   /* offset 32 */
    uint32_t        index_count;                      /* offset 36 */
    float           alpha;                            /* offset 40: sub-frame simulation interpolation */
    uint32_t        flags;                            /* offset 44: bit 0: interp, bit 1: prev, bit 2: hiz, bit 3: late */
    alignas(16) float eye_fov[4];                     /* offset 48: xyz = eye pos, w = fov_y */
    alignas(16) float target_aspect[4];               /* offset 64: xyz = target, w = aspect */
    alignas(16) float up_znear[4];                    /* offset 80: xyz = up vector, w = z_near */
    VkDeviceAddress visibility_addr;                  /* offset 96: BDA: uint32_t* visibility tracking */
    uint32_t        hiz_width;                        /* offset 104 */
    uint32_t        hiz_height;                       /* offset 108 */
    uint32_t        hiz_mips;                         /* offset 112 */
    uint32_t        pad;                              /* offset 116 */
    uint64_t        reserved;                         /* offset 120 */
} khr_hiz_cull_push_t;

static_assert(sizeof(khr_hiz_cull_push_t) == 128, "HiZCullPush size must be 128 bytes");
static_assert(sizeof(khr_hiz_cull_push_t) <= 128, "HiZCullPush must fit within 128 bytes");

/*
 * Hierarchical-Z Depth Pyramid & Two-Pass Occlusion Culler (Pillar 5).
 * Downsamples hardware depth buffer down to 1x1 using conservative MIN reduction for Reversed-Z.
 */
typedef struct {
    const khr_gfx_device_t* dev;
    uint32_t width;
    uint32_t height;
    uint32_t mip_levels;

    /* Depth Pyramid GPU Resources */
    VkImage        image;
    VkDeviceMemory memory;
    VkImageView    view_full;
    VkImageView    mip_views[KHR_HIZ_MAX_MIP_LEVELS];
    VkSampler      sampler;

    /* Compute Downsampler Pipeline */
    VkDescriptorSetLayout downsample_ds_layout;
    VkPipelineLayout      downsample_pipe_layout;
    VkPipeline            downsample_pipeline;
    VkShaderModule        downsample_module;

    /* Hi-Z Occlusion Culling Pipeline */
    VkDescriptorSetLayout cull_ds_layout;
    VkPipelineLayout      cull_pipe_layout;
    VkPipeline            cull_pipeline;
    VkShaderModule        cull_module;

    /* Descriptors */
    VkDescriptorPool desc_pool;
    VkDescriptorSet  downsample_sets[KHR_HIZ_MAX_MIP_LEVELS];
    VkDescriptorSet  cull_set;

    bool initialized;
} khr_hiz_t;

/*
 * Initialize the Hi-Z pyramid and allocate GPU resources and compute pipelines.
 */
[[nodiscard]]
bool khr_hiz_init(khr_hiz_t* hiz,
                  const khr_gfx_device_t* dev,
                  uint32_t width,
                  uint32_t height);

/*
 * Destroy Hi-Z resources, pipelines, and descriptor sets.
 */
void khr_hiz_destroy(khr_hiz_t* hiz);

/*
 * Build the full Hi-Z mip chain down to 1x1 from the rendered depth buffer.
 * Copies mip 0 from src_depth, then dispatches downsample compute passes.
 */
void khr_hiz_build(const khr_hiz_t* hiz,
                   VkCommandBuffer cmd,
                   VkImage src_depth,
                   VkImageLayout current_depth_layout);

/*
 * Dispatch compute occlusion culling pass (Early or Late pass).
 */
void khr_hiz_cull_dispatch(const khr_hiz_t* hiz,
                           VkCommandBuffer cmd,
                           const khr_hiz_cull_push_t* push);

#endif /* KHOROS_GFX_HIZ_H */

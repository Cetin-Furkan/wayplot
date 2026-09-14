#ifndef KHOROS_GFX_CULL_PIPELINE_H
#define KHOROS_GFX_CULL_PIPELINE_H

#if __STDC_VERSION__ < 202311L
#error "Khoros requires pure ISO C23 (compile with -std=c23)"
#endif

#ifdef _STDBOOL_H
#error "<stdbool.h> is strictly banned. In C23, bool, true, and false are native language keywords."
#endif

#ifdef _STDALIGN_H
#error "<stdalign.h> is strictly banned. In C23, alignas and alignof are native language keywords."
#endif

#include <vulkan/vulkan.h>
#include "khoros/core/attributes.h"
#include "khoros/gfx/device.h"
#include "khoros/gfx/gpu_math.h"
#include "khoros/gfx/descriptor_buffer.h"

/*
 * Compute Pipeline for GPU-Driven Frustum Culling & Math
 */
typedef struct {
    const khr_gfx_device_t* dev;
    VkPipelineLayout        layout;
    VkPipeline              pipeline;
    VkShaderModule          comp_module;
} khr_cull_pipeline_t;

/*
 * Graphics Pipeline for GPU-Driven Instanced Mesh Rendering
 */
typedef struct {
    const khr_gfx_device_t* dev;
    VkPipelineLayout        layout;
    VkPipeline              pipeline;
    VkShaderModule          vs_module;
    VkShaderModule          fs_module;
    VkFormat                color_format;
    VkDescriptorSetLayout   resource_layout;
    VkDescriptorSetLayout   sampler_layout;
    bool                    owns_layouts;
} khr_mesh_instanced_pipeline_t;

[[nodiscard]]
bool khr_cull_pipeline_init(khr_cull_pipeline_t* p, const khr_gfx_device_t* d);

void khr_cull_pipeline_destroy(khr_cull_pipeline_t* p);

void khr_cull_pipeline_dispatch(const khr_cull_pipeline_t* p,
                                VkCommandBuffer cmd,
                                const khr_cull_push_t* push);

[[nodiscard]]
bool khr_mesh_instanced_pipeline_init(khr_mesh_instanced_pipeline_t* p,
                                      const khr_gfx_device_t* d,
                                      VkFormat color_format,
                                      const khr_descriptor_heap_t* heap);

void khr_mesh_instanced_pipeline_destroy(khr_mesh_instanced_pipeline_t* p);

void khr_mesh_instanced_draw_indirect(const khr_mesh_instanced_pipeline_t* p,
                                      VkCommandBuffer cmd,
                                      const khr_mesh_instanced_push_t* push,
                                      VkBuffer indirect_buffer,
                                      VkDeviceSize indirect_offset,
                                      uint32_t draw_count,
                                      uint32_t stride);

#endif /* KHOROS_GFX_CULL_PIPELINE_H */

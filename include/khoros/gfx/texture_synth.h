#ifndef KHOROS_GFX_TEXTURE_SYNTH_H
#define KHOROS_GFX_TEXTURE_SYNTH_H

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
#include "khoros/gfx/texture.h"
#include <stdint.h>
#include <vulkan/vulkan.h>

/*
 * Procedural PBR Texture Generation Patterns (Zero-CPU, 100% GPU Compute).
 */
typedef enum {
    KHR_TEX_SYNTH_DAMASCUS_STEEL = 0, /* Woven metal flow & carbide banding */
    KHR_TEX_SYNTH_BRUSHED_METAL   = 1, /* Anodized micro-grooves and directional grain */
    KHR_TEX_SYNTH_NORMAL_MAP      = 2, /* Tangent-space normal map derived via GPU gradients */
    KHR_TEX_SYNTH_PBR_CHECKER     = 3, /* High-contrast PBR calibration grid */
    KHR_TEX_SYNTH_MARBLE          = 4, /* Veined Italian Carrara marble & polished stone */
    KHR_TEX_SYNTH_CARBON_FIBER    = 5, /* Woven carbon fiber & ballistic kevlar weave */
} khr_tex_synth_pattern_t;

/*
 * Push constants matching shaders/tex_synthesizer.slang TexSynthPush layout.
 */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pattern;
    float    frequency;
    float    tint[4];
} khr_tex_synth_push_t;

/*
 * GPU-Driven Texture Synthesizer Pipeline (Pillar 1).
 * Directly synthesizes PBR textures and normal maps into device-local tiled VRAM
 * using VK_EXT_descriptor_buffer without legacy descriptor pools or CPU decoding.
 */
typedef struct {
    const khr_gfx_device_t* dev;
    VkPipelineLayout        layout;
    VkPipeline              pipeline;
    VkShaderModule          comp_module;
    VkDescriptorSetLayout   desc_layout;
    VkDeviceSize            layout_size;
    VkDeviceSize            binding_offset;

    /* Single-descriptor buffer backing for storage image target */
    VkBuffer                desc_buffer;
    VkDeviceMemory          desc_memory;
    void*                   desc_host_map;
    VkDeviceAddress         desc_bda;
    size_t                  desc_total_bytes;
    uint32_t                desc_size;

    bool initialized;
} khr_texture_synth_pipeline_t;

/*
 * Initialize the GPU texture synthesizer compute pipeline.
 */
[[nodiscard]]
bool khr_texture_synth_pipeline_init(khr_texture_synth_pipeline_t* p,
                                     const khr_gfx_device_t* dev);

/*
 * Teardown the GPU texture synthesizer pipeline and its descriptor resources.
 */
void khr_texture_synth_pipeline_destroy(khr_texture_synth_pipeline_t* p);

/*
 * Record synthesis commands into an active command buffer.
 * Transitions tex to GENERAL, writes descriptor directly into descriptor buffer,
 * executes compute dispatch, and transitions tex to SHADER_READ_ONLY_OPTIMAL.
 */
void khr_texture_synth_record_dispatch(khr_texture_synth_pipeline_t* p,
                                       VkCommandBuffer cmd,
                                       khr_texture_t* tex,
                                       khr_tex_synth_pattern_t pattern,
                                       float frequency,
                                       const float tint[4]);

/*
 * Standalone one-shot synchronous helper:
 * Creates an optimal device-local texture, synthesizes it on GPU compute, and waits for completion.
 */
[[nodiscard]]
bool khr_texture_synth_generate_2d(khr_texture_synth_pipeline_t* p,
                                   khr_texture_t* out_tex,
                                   uint32_t width,
                                   uint32_t height,
                                   khr_tex_synth_pattern_t pattern,
                                   float frequency,
                                   const float tint[4]);

#endif /* KHOROS_GFX_TEXTURE_SYNTH_H */

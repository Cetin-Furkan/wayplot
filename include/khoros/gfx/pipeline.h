#ifndef KHOROS_GFX_PIPELINE_H
#define KHOROS_GFX_PIPELINE_H

#if __STDC_VERSION__ < 202311L
#error "Khoros requires pure ISO C23 (compile with -std=c23)"
#endif

#ifdef _STDBOOL_H
#error "<stdbool.h> is strictly banned. In C23, bool, true, and false are native language keywords."
#endif

#ifdef _STDALIGN_H
#error "<stdalign.h> is strictly banned. In C23, alignas and alignof are native language keywords."
#endif

#include "khoros/gfx/device.h"
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

constexpr uint32_t KHR_PUSH_CONSTANT_MAX_BYTES = 128;

typedef struct khr_card_instance {
    float rect[4];          /* x, y, w, h in pixels */
    uint32_t bg_rgba;       /* packed 32-bit RGBA */
    uint32_t border_rgba;   /* packed 32-bit RGBA */
    float corner_radius;    /* pixels */
    float border_width;     /* pixels */
} khr_card_instance_t;

typedef struct khr_card_push {
    VkDeviceAddress cards_addr; /* 8 bytes */
    float screen_extent[2];     /* 8 bytes */
    float scale;                /* 4 bytes */
    uint32_t card_index;        /* 4 bytes */
} khr_card_push_t;

typedef struct khr_plot_push {
    float mvp_c0[4];            /* offset 0..15 */
    float mvp_c1[4];            /* offset 16..31 */
    float mvp_c2[4];            /* offset 32..47 */
    float mvp_c3[4];            /* offset 48..63 */
    float light_dir[4];         /* offset 64..79 */
    VkDeviceAddress samples_addr; /* offset 80..87 */
    uint32_t count;             /* offset 88..91 */
    float amp;                  /* offset 92..95 */
    float half_w;               /* offset 96..99 */
    uint32_t pad0;              /* offset 100..103 */
    uint32_t pad1;              /* offset 104..107 */
    uint32_t pad2;              /* offset 108..111 */
} khr_plot_push_t;

static_assert(sizeof(khr_card_instance_t) == 32, "CardInstance size must be 32 bytes");
static_assert(sizeof(khr_card_push_t) == 24, "CardPush size must be 24 bytes");
static_assert(sizeof(khr_plot_push_t) == 112, "PlotPush size must be 112 bytes");
static_assert(sizeof(khr_plot_push_t) <= 128, "PlotPush must fit 128-byte hardware limit");

/* Demo series living in the BDA arena. File ingest is a later step. */
constexpr uint32_t KHR_PLOT_SAMPLE_COUNT = 256;
constexpr float    KHR_PLOT_AMP          = 0.72f;
constexpr float    KHR_PLOT_HALF_W       = 0.05f;

typedef struct khr_shader_bytecode {
    const uint32_t *code;
    size_t size_bytes;
} khr_shader_bytecode_t;

typedef struct {
    VkShaderModule      vs_module;
    const char*         vs_entry;     /* If nullptr, defaults to "vs_main" */
    VkShaderModule      fs_module;
    const char*         fs_entry;     /* If nullptr, defaults to "fs_main" */
    VkPipelineLayout    layout;
    VkFormat            color_format;
    VkFormat            depth_format;  /* VK_FORMAT_UNDEFINED if depth disabled */
    bool                blend_enable;
    VkCullModeFlags     cull_mode;     /* e.g. VK_CULL_MODE_NONE */
    VkPrimitiveTopology topology;      /* e.g. VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST */
} khr_gfx_pipeline_config_t;

typedef struct khr_card_pipeline {
    const khr_gfx_device_t *dev;
    VkShaderModule vs_module;
    VkShaderModule fs_module;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    VkFormat color_format;
} khr_card_pipeline_t;

typedef struct khr_plot_pipeline {
    const khr_gfx_device_t *dev;
    VkShaderModule vs_module;
    VkShaderModule fs_module;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    VkFormat color_format;
} khr_plot_pipeline_t;

[[nodiscard]] khr_shader_bytecode_t khr_shader_get_card_vert(void);
[[nodiscard]] khr_shader_bytecode_t khr_shader_get_card_frag(void);
[[nodiscard]] khr_shader_bytecode_t khr_shader_get_plot_vert(void);
[[nodiscard]] khr_shader_bytecode_t khr_shader_get_plot_frag(void);

[[nodiscard]]
bool khr_pipeline_layout_create(VkDevice dev,
                                VkDescriptorSetLayout set_layout,
                                uint32_t push_size,
                                VkPipelineLayout* out_layout);

void khr_pipeline_layout_destroy(VkDevice dev, VkPipelineLayout layout);

[[nodiscard]]
bool khr_shader_module_create(VkDevice dev,
                              const uint32_t* spv_code,
                              size_t code_size,
                              VkShaderModule* out_module);

void khr_shader_module_destroy(VkDevice dev, VkShaderModule module);

[[nodiscard]]
bool khr_gfx_pipeline_create(VkDevice dev,
                             const khr_gfx_pipeline_config_t* cfg,
                             VkPipeline* out_pipeline);

void khr_gfx_pipeline_destroy(VkDevice dev, VkPipeline pipeline);

[[nodiscard]] bool khr_card_pipeline_init(khr_card_pipeline_t *p, const khr_gfx_device_t *d, VkFormat color_format);
void khr_card_pipeline_destroy(khr_card_pipeline_t *p);
void khr_card_draw(const khr_card_pipeline_t *p, VkCommandBuffer cmd, const khr_card_push_t *push, uint32_t card_count);

[[nodiscard]] bool khr_plot_pipeline_init(khr_plot_pipeline_t *p, const khr_gfx_device_t *d, VkFormat color_format);
void khr_plot_pipeline_destroy(khr_plot_pipeline_t *p);
void khr_plot_draw(const khr_plot_pipeline_t *p, VkCommandBuffer cmd, const khr_plot_push_t *push, uint32_t segment_count);

/* Host-side demo series. Values stay in (0, 1) so the shader color ramp is used. */
void khr_plot_fill_demo_samples(float* samples, uint32_t count);

#endif /* KHOROS_GFX_PIPELINE_H */

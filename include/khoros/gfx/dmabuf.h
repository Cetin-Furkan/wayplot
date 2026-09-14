#ifndef KHOROS_GFX_DMABUF_H
#define KHOROS_GFX_DMABUF_H

#if __STDC_VERSION__ < 202311L
#error "Khoros requires pure ISO C23 (compile with -std=c23)"
#endif

#ifdef _STDBOOL_H
#error "<stdbool.h> is strictly banned. In C23, bool, true, and false are native language keywords."
#endif

#ifdef _STDALIGN_H
#error "<stdalign.h> is strictly banned. In C23, alignas and alignof are native language keywords."
#endif

#include <stdint.h>
#include "khoros/core/attributes.h"
#include "khoros/gfx/device.h"
#include "khoros/gfx/pipeline.h"
#include "khoros/gfx/bda_arena.h"
#include "khoros/gfx/depth.h"
#include "khoros/gfx/cull_pipeline.h"
#include "khoros/gfx/hiz.h"
#include "khoros/gfx/grid.h"

constexpr VkFormat KHR_DMABUF_VK_FORMAT = VK_FORMAT_B8G8R8A8_UNORM;

/*
 * DMA-BUF-exportable Vulkan image (no WSI). The driver chooses the tiling;
 * the actual modifier is queried back (never hardcoded per-vendor) and handed
 * to the Wayland import verbatim, with row pitch from the subresource layout.
 * Single-plane B8G8R8A8_UNORM (DRM_FORMAT_ARGB8888) in this milestone.
 */
typedef struct {
    VkImage    image;
    VkDeviceMemory mem;
    VkImageTiling tiling; /* winner of the exportability probe */
    uint32_t   w;
    uint32_t   h;
    uint32_t   drm_format;
    uint64_t   modifier;
    uint32_t   stride;
    uint32_t   offset;
    int        dma_fd; /* -1 until khr_dmabuf_image_export succeeds */
} khr_dmabuf_image_t;

[[nodiscard]]
bool khr_dmabuf_image_init(khr_gfx_device_t* d, khr_dmabuf_image_t* img,
                           uint32_t w, uint32_t h);

[[nodiscard]]
bool khr_dmabuf_image_init_with_modifiers(khr_gfx_device_t* d, khr_dmabuf_image_t* img,
                                          uint32_t w, uint32_t h,
                                          const uint64_t* wayland_modifiers,
                                          uint32_t wayland_modifier_count);

[[nodiscard]]
bool khr_dmabuf_image_export(khr_gfx_device_t* d, khr_dmabuf_image_t* img);

void khr_dmabuf_image_destroy(khr_gfx_device_t* d, khr_dmabuf_image_t* img);

/*
 * Renderable swapchain slot: an exportable image plus everything needed to
 * paint one GPU frame into it without any host wait. The pipeline is created
 * once per slot (never per frame); the layout is tracked across frames
 * (UNDEFINED first, GENERAL after each render — the export layout the
 * compositor reads). Submit signals the caller's semaphore at the given
 * timeline value; completion is observed via counter queries, never waits.
 */
typedef struct {
    khr_dmabuf_image_t img;
    VkImageView        view;
    VkCommandPool      pool;
    VkCommandBuffer    cmd;
    khr_card_pipeline_t pipe;
    bool               pipe_live;
    bool               owns_pipe;
    PFN_vkCmdPushConstants2KHR pfn_push2;
    VkImageLayout      layout;
    uint64_t           painted;
    /* MSAA color + depth are GPU-local. Resolve into img (the DMA-BUF). */
    VkSampleCountFlagBits samples;
    VkFormat           depth_format;
    VkImage            msaa_color;
    VkDeviceMemory     msaa_color_mem;
    VkImageView        msaa_color_view;
    VkImageLayout      msaa_color_layout;
    khr_depth_target_t depth_target;
    VkImageLayout      depth_layout;
} khr_dmabuf_slot_t;

/* Image + DMA-BUF export + view + command pool/buffer + card pipeline. */
[[nodiscard]]
bool khr_dmabuf_slot_init(khr_gfx_device_t* d, khr_dmabuf_slot_t* slot,
                          uint32_t w, uint32_t h);

[[nodiscard]]
bool khr_dmabuf_slot_init_shared(khr_gfx_device_t* d, khr_dmabuf_slot_t* slot,
                                 uint32_t w, uint32_t h,
                                 const khr_card_pipeline_t* shared_pipe);

[[nodiscard]]
bool khr_dmabuf_slot_init_shared_with_modifiers(khr_gfx_device_t* d, khr_dmabuf_slot_t* slot,
                                                uint32_t w, uint32_t h,
                                                const khr_card_pipeline_t* shared_pipe,
                                                const uint64_t* modifiers,
                                                uint32_t modifier_count);

/*
 * Paint one frame (fullscreen card, frame-varying color for visible cadence)
 * and submit signalling (signal_sem, signal_value). Returns once submitted;
 * the image is in GENERAL layout and must not be touched until the signal
 * fires (polled via vkGetSemaphoreCounterValue, never vkWaitSemaphores on a
 * hot path).
 */
[[nodiscard]]
bool khr_dmabuf_slot_render(khr_gfx_device_t* d, khr_dmabuf_slot_t* slot,
                            khr_bda_arena_t* arena, uint64_t frame_no,
                            VkSemaphore signal_sem, uint64_t signal_value);

/* Draw card_count instances already in the BDA arena. Does not bump-alloc. */
[[nodiscard]]
bool khr_dmabuf_slot_render_cards(khr_gfx_device_t* d, khr_dmabuf_slot_t* slot,
                                  VkDeviceAddress cards_addr, uint32_t card_count,
                                  VkSemaphore signal_sem, uint64_t signal_value);

typedef struct khr_gizmo_pass {
    VkDeviceAddress verts_addr;
    VkDeviceAddress indices_addr;
    uint32_t        index_count;
    uint32_t        vert_count;
    uint32_t        x;
    uint32_t        y;
    uint32_t        s;
    float           r0[3];
    float           r1[3];
    float           r2[3];
} khr_gizmo_pass_t;

/* GPU-driven scene rendering pass with compute culling and multi-light PBR */
typedef struct khr_gpu_scene_pass {
    const khr_cull_pipeline_t*           cull_pipe;
    const khr_cull_push_t*               cull_push;
    const khr_mesh_instanced_pipeline_t* inst_pipe;
    const khr_mesh_instanced_push_t*     inst_push;
    VkBuffer                             indirect_cmd_buffer;
    VkDeviceSize                         indirect_cmd_offset;
    uint32_t                             draw_count;
    const khr_hiz_t*                     hiz;
    const khr_hiz_cull_push_t*           hiz_push;
    const khr_descriptor_heap_t*         descriptor_heap;
    const khr_grid_pipeline_t*           grid_pipe;
    const khr_grid_push_t*               grid_push;
    uint32_t                             mesh_pass_count;
    khr_mesh_instanced_push_t            mesh_pushes[8];
    VkDeviceSize                         indirect_cmd_offsets[8];
    uint32_t                             draw_counts[8];
} khr_gpu_scene_pass_t;

/* Cards plus mesh (preferred), plot ribbon, or GPU-driven PBR scene in the client rect.
 * top_px insets under the title bar. mesh/plot/gpu_scene/gizmo may be null. */
[[nodiscard]]
bool khr_dmabuf_slot_render_scene(khr_gfx_device_t* d, khr_dmabuf_slot_t* slot,
                                  VkDeviceAddress cards_addr, uint32_t card_count,
                                  const khr_plot_pipeline_t* plot,
                                  const khr_plot_push_t* plot_push,
                                  const khr_mesh_pipeline_t* mesh,
                                  const khr_mesh_push_t* mesh_push,
                                  const khr_gpu_scene_pass_t* gpu_scene,
                                  const khr_gizmo_pass_t* gizmo,
                                  uint32_t plot_top_px,
                                  VkSemaphore signal_sem, uint64_t signal_value);

void khr_dmabuf_slot_destroy(khr_gfx_device_t* d, khr_dmabuf_slot_t* slot);

#endif /* KHOROS_GFX_DMABUF_H */

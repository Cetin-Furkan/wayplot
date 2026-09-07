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
    PFN_vkCmdPushConstants2KHR pfn_push2;
    VkImageLayout      layout;
    uint64_t           painted;
} khr_dmabuf_slot_t;

/* Image + DMA-BUF export + view + command pool/buffer + card pipeline. */
[[nodiscard]]
bool khr_dmabuf_slot_init(khr_gfx_device_t* d, khr_dmabuf_slot_t* slot,
                          uint32_t w, uint32_t h);

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

void khr_dmabuf_slot_destroy(khr_gfx_device_t* d, khr_dmabuf_slot_t* slot);

#endif /* KHOROS_GFX_DMABUF_H */

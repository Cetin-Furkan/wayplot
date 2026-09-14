#ifndef KHOROS_GFX_FRAME_H
#define KHOROS_GFX_FRAME_H

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
 * Real Vulkan 1.4 offscreen frame (no WSI, no libwayland: presentation travels
 * over the io_uring Wayland socket, never through a swapchain extension).
 *
 * One render proves the full device path that the old facade test only
 * recorded without submitting:
 * - dynamic rendering (vkCmdBeginRendering / vkCmdEndRendering)
 * - dynamic viewport + scissor state (the pipeline enables both)
 * - host -> shader BDA coherence barrier (HOST_WRITE to SHADER_READ)
 * - BDA push constants (maintenance6 push-constants2 record when available)
 * - vkQueueSubmit2 with a timeline semaphore + host vkWaitSemaphores
 * - readback via host image copy when enabled, transfer copy otherwise
 * - mutex-serialized queue submission (shared VkQueue on UMA silicon)
 */
constexpr uint32_t KHR_FRAME_DEFAULT_W = 256;
constexpr uint32_t KHR_FRAME_DEFAULT_H = 256;

typedef struct {
    VkImage         image;
    VkDeviceMemory  image_mem;
    VkImageView     view;
    VkBuffer        readback;
    VkDeviceMemory  readback_mem;
    void*           readback_host;
    size_t          readback_sz;
    VkCommandPool   pool;
    VkCommandBuffer cmd;
    VkSemaphore     timeline;
    uint64_t        next_point;
    uint32_t        w;
    uint32_t        h;
    bool            use_host_copy;
    /* Vulkan 1.4-era entry points missing from older loaders: resolved per
     * device, with in-pipeline fallbacks when absent (classic push constants,
     * transfer-engine copy). Presence is asserted by the frame test on 1.4
     * hardware; absence never silently degrades on paper. */
    PFN_vkCmdPushConstants2KHR pfn_push2;
    PFN_vkCopyImageToMemoryEXT pfn_host_copy;
} khr_gfx_frame_t;

[[nodiscard]]
bool khr_gfx_frame_init(khr_gfx_device_t* d, khr_gfx_frame_t* f,
                        uint32_t w, uint32_t h);

void khr_gfx_frame_destroy(khr_gfx_device_t* d, khr_gfx_frame_t* f);

/*
 * Render one fullscreen opaque red card from a BDA arena instance and read
 * back the center pixel as BGRA bytes. Returns true with out_bgra set; false
 * on any Vulkan failure (the test then fails honestly, never vacuously).
 */
[[nodiscard]]
bool khr_gfx_frame_render_red_card(khr_gfx_device_t* d, khr_gfx_frame_t* f,
                                   khr_bda_arena_t* arena, uint8_t out_bgra[4]);

/*
 * Render two overlapping triangles (one at Z=0.2, one at Z=0.8) with depth test.
 * If reverse_order is false: render Far (Z=0.2) first, Near (Z=0.8) second.
 * If reverse_order is true: render Near (Z=0.8) first, Far (Z=0.2) second.
 * With Reversed-Z (depth test >=, clear 0.0), both orderings must resolve to the
 * nearer triangle's color (Green).
 */
[[nodiscard]]
bool khr_gfx_frame_render_depth_test(khr_gfx_device_t* d, khr_gfx_frame_t* f,
                                     khr_bda_arena_t* arena, bool reverse_order,
                                     uint8_t out_bgra[4]);

#endif /* KHOROS_GFX_FRAME_H */

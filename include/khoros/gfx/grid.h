#ifndef KHOROS_GFX_GRID_H
#define KHOROS_GFX_GRID_H

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
#include <stdint.h>
#include <vulkan/vulkan.h>

/*
 * Push constants for shaders/grid.slang.
 * 64 bytes total, matching SPIR-V std430 layout.
 */
typedef struct {
    alignas(16) float eye_plane_y[4];     /* xyz = eye pos, w = plane height (Y) */
    alignas(16) float target_minor_sz[4]; /* xyz = target pos, w = minor grid cell size */
    alignas(16) float up_major_sz[4];     /* xyz = up vector, w = major grid cell size */
    alignas(16) float params[4];          /* x = fov_y, y = aspect, z = z_near, w = max_dist */
} khr_grid_push_t;

static_assert(sizeof(khr_grid_push_t) == 64, "Grid push constant layout must be 64 bytes");
static_assert(sizeof(khr_grid_push_t) <= 128, "Grid push constant must fit Vulkan 128-byte guarantee");

/*
 * Infinite Anti-Aliased Ground Grid Graphics Pipeline (Pillar 3).
 * Uses Vulkan 1.4 dynamic rendering with alpha blending and reversed-Z depth testing.
 */
typedef struct {
    const khr_gfx_device_t* dev;
    VkPipelineLayout        layout;
    VkPipeline              pipeline;
} khr_grid_pipeline_t;

[[nodiscard]]
bool khr_grid_pipeline_init(khr_grid_pipeline_t* p,
                            const khr_gfx_device_t* dev,
                            VkFormat color_fmt,
                            VkFormat depth_fmt);

void khr_grid_pipeline_destroy(khr_grid_pipeline_t* p);

void khr_grid_pipeline_draw(const khr_grid_pipeline_t* p,
                            VkCommandBuffer cmd,
                            const khr_grid_push_t* push);

#endif /* KHOROS_GFX_GRID_H */

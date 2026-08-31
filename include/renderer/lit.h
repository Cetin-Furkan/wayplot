#ifndef RENDERER_LIT_H
#define RENDERER_LIT_H

#include "renderer/camera.h"
#include "renderer/image.h"
#include "renderer/mesh.h"
#include "vulkan/device.h"

#include <stdint.h>
#include <vulkan/vulkan.h>

#ifndef WP_SWAPCHAIN_IMAGES
#define WP_SWAPCHAIN_IMAGES 3
#endif

/* Per swapchain slot. Matches WP_DRAW_MAX. UBO stride is 256 (Vulkan max
 * minUniformBufferOffsetAlignment). Call wp_lit_reset at opaque begin. */
#define WP_LIT_MAX_DRAWS 64

/*
 * One lit-mesh pass: vertex color * directional light * albedo.
 * Not a cube object. Any wp_mesh with wp_vn_vertex layout can be drawn.
 *
 * Culling: BACK faces. Front face follows wp_camera_front_clockwise()
 * (false → COUNTER_CLOCKWISE: see camera.h / Vulkan `a = -shoelace`).
 * A triangle is rasterized only when the camera sees its outward side.
 * Never CULL_NONE. Must run inside wp_pass_opaque_begin/end.
 */

struct wp_lit {
    struct wp_device *dev;
    VkShaderModule vs;
    VkShaderModule fs;
    VkPipeline pipeline;
    VkFormat color_fmt;
    struct wp_buffer ubo[WP_SWAPCHAIN_IMAGES];
    uint32_t ubo_used[WP_SWAPCHAIN_IMAGES];
    VkImage albedo;
    VkImageView albedo_view;
    VkDeviceMemory albedo_mem;
    VkSampler sampler;
    VkImageLayout albedo_layout;
};

[[nodiscard]] int wp_lit_init(struct wp_lit *l, struct wp_device *d, VkFormat color_fmt);
void wp_lit_reset(struct wp_lit *l, uint32_t slot);
void wp_lit_draw(struct wp_lit *l, VkCommandBuffer cmd, uint32_t width, uint32_t height,
                 uint32_t slot, const struct wp_mesh *mesh, const struct wp_camera *cam,
                 const float model[16]);
/* tex NULL uses the default 1×1 white albedo. */
void wp_lit_draw_tex(struct wp_lit *l, VkCommandBuffer cmd, uint32_t width, uint32_t height,
                     uint32_t slot, const struct wp_mesh *mesh, const struct wp_camera *cam,
                     const float model[16], const struct wp_tex *tex);
void wp_lit_destroy(struct wp_lit *l);

#endif /* RENDERER_LIT_H */

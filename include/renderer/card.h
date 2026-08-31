#ifndef RENDERER_CARD_H
#define RENDERER_CARD_H

#include "vulkan/buffer.h"
#include "vulkan/device.h"

#include <stdint.h>
#include <vulkan/vulkan.h>

#ifndef WP_SWAPCHAIN_IMAGES
#define WP_SWAPCHAIN_IMAGES 3
#endif

/*
 * Overlay rect. Pixel space, Y down, same winding as text (TL, BL, BR, TR).
 * Not a fullscreen panel shader. Not a 3D lit mesh. See docs/CARD.md.
 *
 * Must run inside wp_pass_overlay_begin/end. Call wp_card_reset at overlay
 * begin so a second card does not overwrite the first.
 */

#define WP_CARD_VERTS 4
#define WP_CARD_INDS 6
#define WP_CARD_MAX_DRAWS 64

/* Source of truth for a card. Geom is derived (wp_card_cpu). Pixel space. */
struct wp_rect {
    float x, y, w, h;
};

struct wp_card_vertex {
    float x, y;
};

struct wp_card_geom {
    struct wp_card_vertex v[WP_CARD_VERTS];
    uint16_t idx[WP_CARD_INDS];
    float x0, y0, x1, y1;
};

struct wp_card {
    struct wp_device *dev;
    VkShaderModule vs;
    VkShaderModule fs;
    VkPipeline pipeline;
    VkFormat color_fmt;
    struct wp_buffer ubo[WP_SWAPCHAIN_IMAGES];
    struct wp_buffer vbo[WP_SWAPCHAIN_IMAGES];
    struct wp_buffer ibo[WP_SWAPCHAIN_IMAGES];
    uint32_t ubo_used[WP_SWAPCHAIN_IMAGES];
    uint32_t vused[WP_SWAPCHAIN_IMAGES];
    uint32_t iused[WP_SWAPCHAIN_IMAGES];
};

[[nodiscard]] int wp_rect_ok(const struct wp_rect *r);
struct wp_rect wp_rect_scaled(struct wp_rect r, float s);

[[nodiscard]] int wp_card_cpu(float x, float y, float w, float h, struct wp_card_geom *g);

[[nodiscard]] int wp_card_init(struct wp_card *c, struct wp_device *d, VkFormat color_fmt);
void wp_card_reset(struct wp_card *c, uint32_t slot);
void wp_card_draw(struct wp_card *c, VkCommandBuffer cmd, uint32_t width, uint32_t height,
                  uint32_t slot, const struct wp_card_geom *geom, const float rgba[4]);
void wp_card_destroy(struct wp_card *c);

#endif /* RENDERER_CARD_H */

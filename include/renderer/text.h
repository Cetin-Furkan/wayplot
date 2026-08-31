#ifndef RENDERER_TEXT_H
#define RENDERER_TEXT_H

#include "renderer/font.h"
#include "vulkan/buffer.h"
#include "vulkan/device.h"

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifndef WP_SWAPCHAIN_IMAGES
#define WP_SWAPCHAIN_IMAGES 3
#endif

#define WP_TEXT_MAX_GLYPHS 2048
/* Per swapchain slot. Matches WP_DRAW_MAX. UBO stride is 256. Call
 * wp_text_reset at overlay begin so a second run does not overwrite the first. */
#define WP_TEXT_MAX_DRAWS 64

struct wp_text_vertex {
    float x, y, u, v;
};

/* CPU layout in pixel space (Y down, origin = top-left of first line). */
struct wp_text_geom {
    struct wp_text_vertex *v;
    uint16_t *idx;
    uint32_t nv;
    uint32_t ni;
    float x0, y0, x1, y1;
};

/*
 * Overlay pass: any laid-out run. Not a label widget and not a string.
 * Blend, no depth. Front face from wp_camera_front_clockwise() (CCW).
 * Must run inside wp_pass_overlay_begin/end. Does not BeginRendering.
 */
struct wp_text {
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

void wp_text_geom_free(struct wp_text_geom *g);
[[nodiscard]] int wp_text_layout(const struct wp_font *f, const char *utf8,
                                 float origin_x, float origin_y, struct wp_text_geom *g);

[[nodiscard]] int wp_text_init(struct wp_text *t, struct wp_device *d, VkFormat color_fmt);
void wp_text_reset(struct wp_text *t, uint32_t slot);
void wp_text_draw(struct wp_text *t, VkCommandBuffer cmd, uint32_t width, uint32_t height,
                  uint32_t slot, const struct wp_font *font, const struct wp_text_geom *geom,
                  const float rgba[4]);
void wp_text_destroy(struct wp_text *t);

#endif /* RENDERER_TEXT_H */
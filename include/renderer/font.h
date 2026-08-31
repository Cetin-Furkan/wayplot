#ifndef RENDERER_FONT_H
#define RENDERER_FONT_H

#include "vulkan/device.h"

#include <stdint.h>
#include <vulkan/vulkan.h>

#define WP_FONT_CP0 32
#define WP_FONT_N 95 /* U+0020 .. U+007E */

struct wp_glyph {
    float advance;
    float left;  /* bitmap_left */
    float top;   /* bitmap_top from baseline (FreeType) */
    float w, h;
    float u0, v0, u1, v1;
    int present;
};

struct wp_font {
    uint8_t *atlas;
    uint32_t atlas_w;
    uint32_t atlas_h;
    float size_px;
    float ascent;
    float descent;
    float line_height;
    struct wp_glyph glyph[WP_FONT_N];
    struct wp_glyph missing;
    int16_t kern[WP_FONT_N][WP_FONT_N]; /* 26.6 pixels; 0 if none */

    struct wp_device *dev;
    VkImage image;
    VkImageView view;
    VkDeviceMemory mem;
    VkSampler sampler;
    VkImageLayout layout;
};

[[nodiscard]] int wp_font_open(struct wp_font *f, const char *path, float size_px);
[[nodiscard]] int wp_font_open_default(struct wp_font *f, float size_px);
[[nodiscard]] int wp_font_upload(struct wp_font *f, struct wp_device *d);
const struct wp_glyph *wp_font_glyph(const struct wp_font *f, uint32_t cp);
float wp_font_kern(const struct wp_font *f, uint32_t a, uint32_t b);
void wp_font_destroy(struct wp_font *f);

#endif /* RENDERER_FONT_H */
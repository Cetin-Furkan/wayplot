#ifndef RENDERER_IMAGE_H
#define RENDERER_IMAGE_H

#include "renderer/mesh.h"
#include "vulkan/device.h"

#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

/*
 * CPU pixels → sampled VkImage. P6 binary RGB. Not a fullscreen blit.
 * See docs/IMAGE.md.
 */

#define WP_IMAGE_MAX_SIDE 2048

struct wp_image {
    uint8_t *rgba; /* tightly packed RGBA8 */
    uint32_t w;
    uint32_t h;
};

struct wp_tex {
    struct wp_device *dev;
    VkImage image;
    VkImageView view;
    VkDeviceMemory mem;
    VkSampler sampler;
    VkImageLayout layout;
    uint32_t w;
    uint32_t h;
};

void wp_image_free(struct wp_image *im);
[[nodiscard]] int wp_image_parse_ppm(const void *data, size_t len, struct wp_image *out);
[[nodiscard]] int wp_image_load(const char *path, struct wp_image *out);

/* Unit XZ quad, y=0, CCW from +Y, UV 0..1, white vertex color. */
[[nodiscard]] int wp_image_quad(struct wp_mesh_cpu *out);
/* Mat under the box: XZ padded, y below min. Both windings. See docs/GROUND.md. */
[[nodiscard]] int wp_image_ground(const struct wp_aabb *box, struct wp_mesh_cpu *out);

[[nodiscard]] int wp_tex_upload(struct wp_device *d, struct wp_tex *t, const struct wp_image *im);
void wp_tex_destroy(struct wp_tex *t);

#endif /* RENDERER_IMAGE_H */

#ifndef ENGINE_DRAW_H
#define ENGINE_DRAW_H

#include "renderer/camera.h"
#include "renderer/card.h"
#include "renderer/font.h"
#include "renderer/lit.h"
#include "renderer/mesh.h"
#include "renderer/pass.h"
#include "renderer/text.h"

#include <stdint.h>
#include <vulkan/vulkan.h>

/*
 * CPU array of draws. Immediate recording into the two wp_pass scopes.
 * Not a scene graph, not bindless, not a document. See docs/LIST.md.
 *
 * Items hold pointers to caller-owned mesh/camera/font/geom. Model, rgba,
 * and card rects are copied. Pointers must live until after record.
 */

#define WP_DRAW_MAX 64

enum wp_draw_kind {
    WP_DRAW_NONE = 0,
    WP_DRAW_LIT,
    WP_DRAW_TEXT,
    WP_DRAW_CARD,
};

struct wp_draw_lit {
    const struct wp_mesh *mesh;
    const struct wp_camera *cam;
    float model[16];
};

struct wp_draw_text {
    const struct wp_font *font;
    const struct wp_text_geom *geom;
    float rgba[4];
};

struct wp_draw_card {
    struct wp_rect rect; /* copied. Tessellated at record. */
    float rgba[4];
};

struct wp_draw_item {
    enum wp_draw_kind kind;
    union {
        struct wp_draw_lit lit;
        struct wp_draw_text text;
        struct wp_draw_card card;
    };
};

struct wp_draw_list {
    struct wp_draw_item *items;
    uint32_t count;
    uint32_t cap;
};

void wp_draw_list_init(struct wp_draw_list *l);
void wp_draw_list_clear(struct wp_draw_list *l);
void wp_draw_list_destroy(struct wp_draw_list *l);

[[nodiscard]] int wp_draw_list_push_lit(struct wp_draw_list *l, const struct wp_mesh *mesh,
                                        const struct wp_camera *cam, const float model[16]);
[[nodiscard]] int wp_draw_list_push_text(struct wp_draw_list *l, const struct wp_font *font,
                                         const struct wp_text_geom *geom, const float rgba[4]);
[[nodiscard]] int wp_draw_list_push_card(struct wp_draw_list *l, const struct wp_rect *rect,
                                         const float rgba[4]);

uint32_t wp_draw_list_count(const struct wp_draw_list *l);
uint32_t wp_draw_list_count_kind(const struct wp_draw_list *l, enum wp_draw_kind kind);

/* Walk one bucket. Caller has already begun the matching pass. Resets
 * the pipeline write heads for this swapchain slot. */
void wp_draw_list_record_opaque(const struct wp_draw_list *l, struct wp_lit *lit,
                                VkCommandBuffer cmd, uint32_t width, uint32_t height,
                                uint32_t slot);
void wp_draw_list_record_overlay(const struct wp_draw_list *l, struct wp_text *text,
                                 struct wp_card *card, VkCommandBuffer cmd, uint32_t width,
                                 uint32_t height, uint32_t slot);

/* Opaque begin (clear) + walk + end, then overlay begin (load) + walk +
 * end. Cards then text (text sits on top). Empty buckets still open the
 * scope so the clear lives in one place. */
void wp_draw_list_record(const struct wp_draw_list *l, struct wp_pass *pass,
                         struct wp_lit *lit, struct wp_text *text, struct wp_card *card,
                         VkCommandBuffer cmd, VkImageView color_view, uint32_t width,
                         uint32_t height, uint32_t slot);

#endif /* ENGINE_DRAW_H */

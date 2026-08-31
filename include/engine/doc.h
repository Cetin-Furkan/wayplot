#ifndef ENGINE_DOC_H
#define ENGINE_DOC_H

#include "renderer/card.h"

#include <stdint.h>

/*
 * CPU owner of live cards, mesh pointers, and plot series pointers.
 * Not ImGui, not ECS, not a scene graph. Does not begin rendering. Mesh
 * GPU objects and plot heaps stay owned by the caller. See docs/DOC.md.
 */

struct wp_mesh;
struct wp_plot;
struct wp_tex;
struct wp_hit_stack;
struct wp_draw_list;

#define WP_DOC_MAX_MESH 8
#define WP_DOC_MAX_PLOT 4
#define WP_DOC_MAX_CARD 64
#define WP_DOC_CAPTION 80

struct wp_doc_card {
    struct wp_rect rect;
    float rgba[4];
    char caption[WP_DOC_CAPTION];
};

struct wp_doc {
    struct wp_mesh *meshes[WP_DOC_MAX_MESH];
    struct wp_tex *albedo[WP_DOC_MAX_MESH]; /* NULL = default 1×1 white */
    uint32_t nmesh;
    struct wp_plot *plots[WP_DOC_MAX_PLOT];
    uint32_t nplot;
    struct wp_doc_card cards[WP_DOC_MAX_CARD];
    uint32_t ncards;
};

void wp_doc_init(struct wp_doc *d);
void wp_doc_clear(struct wp_doc *d);
void wp_doc_destroy(struct wp_doc *d);

[[nodiscard]] int wp_doc_add_mesh(struct wp_doc *d, struct wp_mesh *m);
[[nodiscard]] int wp_doc_set_albedo(struct wp_doc *d, uint32_t mesh_i, struct wp_tex *tex);
[[nodiscard]] int wp_doc_add_plot(struct wp_doc *d, struct wp_plot *p);
[[nodiscard]] int wp_doc_add_card(struct wp_doc *d, struct wp_rect rect, const float rgba[4]);
[[nodiscard]] int wp_doc_set_caption(struct wp_doc *d, uint32_t i, const char *s);
[[nodiscard]] int wp_doc_set_card_rect(struct wp_doc *d, uint32_t i, struct wp_rect rect);
[[nodiscard]] int wp_doc_set_card_xy(struct wp_doc *d, uint32_t i, float x, float y);

/* hit_id 0 is a no-op. Else cards[hit_id-1].x/y = drag.x/y (w/h stay). */
[[nodiscard]] int wp_doc_apply_drag(struct wp_doc *d, uint32_t hit_id, struct wp_rect drag);

struct wp_mesh *wp_doc_mesh(const struct wp_doc *d, uint32_t i);
struct wp_tex *wp_doc_albedo(const struct wp_doc *d, uint32_t i);
struct wp_plot *wp_doc_plot(const struct wp_doc *d, uint32_t i);
const struct wp_doc_card *wp_doc_card(const struct wp_doc *d, uint32_t i);
const char *wp_doc_caption(const struct wp_doc *d, uint32_t i);
uint32_t wp_doc_nmesh(const struct wp_doc *d);
uint32_t wp_doc_nplot(const struct wp_doc *d);
uint32_t wp_doc_ncards(const struct wp_doc *d);

/* Push each card as hit id i+1. Does not clear the stack. */
[[nodiscard]] int wp_doc_fill_hits(const struct wp_doc *d, struct wp_hit_stack *h);

/* Scale then push_card. scale<=0 is treated as 1. */
[[nodiscard]] int wp_doc_push_cards(const struct wp_doc *d, struct wp_draw_list *l, float scale);

#endif /* ENGINE_DOC_H */

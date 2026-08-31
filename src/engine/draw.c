#define _GNU_SOURCE
#include "engine/draw.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static_assert(WP_DRAW_MAX == WP_LIT_MAX_DRAWS, "list cap matches lit UBO slots");
static_assert(WP_DRAW_MAX == WP_TEXT_MAX_DRAWS, "list cap matches text UBO slots");
static_assert(WP_DRAW_MAX == WP_CARD_MAX_DRAWS, "list cap matches card UBO slots");

void wp_draw_list_init(struct wp_draw_list *l)
{
    if (!l)
        return;
    memset(l, 0, sizeof(*l));
}

void wp_draw_list_clear(struct wp_draw_list *l)
{
    if (!l)
        return;
    l->count = 0;
}

void wp_draw_list_destroy(struct wp_draw_list *l)
{
    if (!l)
        return;
    free(l->items);
    memset(l, 0, sizeof(*l));
}

static int ensure(struct wp_draw_list *l, uint32_t extra)
{
    uint32_t need, cap;
    struct wp_draw_item *n;

    if (!l)
        return -EINVAL;
    if (extra == 0)
        return 0;
    if (l->count >= WP_DRAW_MAX || extra > WP_DRAW_MAX - l->count)
        return -ENOSPC;
    need = l->count + extra;
    if (need <= l->cap)
        return 0;
    cap = l->cap ? l->cap : 8;
    while (cap < need) {
        if (cap > WP_DRAW_MAX / 2u) {
            cap = WP_DRAW_MAX;
            break;
        }
        cap *= 2u;
    }
    if (cap > WP_DRAW_MAX)
        cap = WP_DRAW_MAX;
    n = realloc(l->items, (size_t)cap * sizeof(*n));
    if (!n)
        return -ENOMEM;
    l->items = n;
    l->cap = cap;
    return 0;
}

int wp_draw_list_push_lit(struct wp_draw_list *l, const struct wp_mesh *mesh,
                          const struct wp_camera *cam, const float model[16])
{
    struct wp_draw_item *it;
    int ret;

    if (!l || !mesh || !cam || !model)
        return -EINVAL;
    ret = ensure(l, 1);
    if (ret < 0)
        return ret;
    it = &l->items[l->count];
    memset(it, 0, sizeof(*it));
    it->kind = WP_DRAW_LIT;
    it->lit.mesh = mesh;
    it->lit.cam = cam;
    memcpy(it->lit.model, model, sizeof(it->lit.model));
    l->count++;
    return 0;
}

int wp_draw_list_push_text(struct wp_draw_list *l, const struct wp_font *font,
                           const struct wp_text_geom *geom, const float rgba[4])
{
    struct wp_draw_item *it;
    int ret;

    if (!l || !font || !geom || !rgba)
        return -EINVAL;
    ret = ensure(l, 1);
    if (ret < 0)
        return ret;
    it = &l->items[l->count];
    memset(it, 0, sizeof(*it));
    it->kind = WP_DRAW_TEXT;
    it->text.font = font;
    it->text.geom = geom;
    memcpy(it->text.rgba, rgba, sizeof(it->text.rgba));
    l->count++;
    return 0;
}

int wp_draw_list_push_card(struct wp_draw_list *l, const struct wp_rect *rect,
                           const float rgba[4])
{
    struct wp_draw_item *it;
    int ret;

    if (!l || !rgba || !wp_rect_ok(rect))
        return -EINVAL;
    ret = ensure(l, 1);
    if (ret < 0)
        return ret;
    it = &l->items[l->count];
    memset(it, 0, sizeof(*it));
    it->kind = WP_DRAW_CARD;
    it->card.rect = *rect;
    memcpy(it->card.rgba, rgba, sizeof(it->card.rgba));
    l->count++;
    return 0;
}

uint32_t wp_draw_list_count(const struct wp_draw_list *l)
{
    return l ? l->count : 0;
}

uint32_t wp_draw_list_count_kind(const struct wp_draw_list *l, enum wp_draw_kind kind)
{
    uint32_t i, n = 0;
    if (!l || !l->items)
        return 0;
    for (i = 0; i < l->count; i++) {
        if (l->items[i].kind == kind)
            n++;
    }
    return n;
}

void wp_draw_list_record_opaque(const struct wp_draw_list *l, struct wp_lit *lit,
                                VkCommandBuffer cmd, uint32_t width, uint32_t height,
                                uint32_t slot)
{
    uint32_t i;
    if (!l || !lit || !cmd)
        return;
    wp_lit_reset(lit, slot);
    if (!l->items)
        return;
    for (i = 0; i < l->count; i++) {
        const struct wp_draw_item *it = &l->items[i];
        if (it->kind != WP_DRAW_LIT)
            continue;
        wp_lit_draw(lit, cmd, width, height, slot, it->lit.mesh, it->lit.cam, it->lit.model);
    }
}

void wp_draw_list_record_overlay(const struct wp_draw_list *l, struct wp_text *text,
                                 struct wp_card *card, VkCommandBuffer cmd, uint32_t width,
                                 uint32_t height, uint32_t slot)
{
    uint32_t i;
    if (!l || !cmd)
        return;
    if (card)
        wp_card_reset(card, slot);
    if (text)
        wp_text_reset(text, slot);
    if (!l->items)
        return;
    /* Cards first so text sits on top (overlay has no depth). */
    if (card) {
        for (i = 0; i < l->count; i++) {
            const struct wp_draw_item *it = &l->items[i];
            struct wp_card_geom g;
            if (it->kind != WP_DRAW_CARD)
                continue;
            if (wp_card_cpu(it->card.rect.x, it->card.rect.y, it->card.rect.w,
                            it->card.rect.h, &g) < 0)
                continue;
            wp_card_draw(card, cmd, width, height, slot, &g, it->card.rgba);
        }
    }
    if (text) {
        for (i = 0; i < l->count; i++) {
            const struct wp_draw_item *it = &l->items[i];
            if (it->kind != WP_DRAW_TEXT)
                continue;
            wp_text_draw(text, cmd, width, height, slot, it->text.font, it->text.geom, it->text.rgba);
        }
    }
}

void wp_draw_list_record(const struct wp_draw_list *l, struct wp_pass *pass,
                         struct wp_lit *lit, struct wp_text *text, struct wp_card *card,
                         VkCommandBuffer cmd, VkImageView color_view, uint32_t width,
                         uint32_t height, uint32_t slot)
{
    if (!l || !pass || !cmd || !color_view)
        return;
    wp_pass_opaque_begin(pass, cmd, color_view, width, height, slot);
    wp_draw_list_record_opaque(l, lit, cmd, width, height, slot);
    wp_pass_opaque_end(cmd);
    wp_pass_overlay_begin(pass, cmd, color_view, width, height);
    wp_draw_list_record_overlay(l, text, card, cmd, width, height, slot);
    wp_pass_overlay_end(cmd);
}

#define _GNU_SOURCE
#include "engine/doc.h"

#include "engine/draw.h"
#include "engine/hit.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static_assert(WP_DOC_MAX_CARD <= WP_HIT_MAX, "every card can sit on the hit stack");
static_assert(WP_DOC_MAX_CARD <= WP_DRAW_MAX, "every card can sit on the list");

void wp_doc_init(struct wp_doc *d)
{
    if (!d)
        return;
    memset(d, 0, sizeof(*d));
}

void wp_doc_clear(struct wp_doc *d)
{
    if (!d)
        return;
    d->nmesh = 0;
    d->nplot = 0;
    d->ncards = 0;
}

void wp_doc_destroy(struct wp_doc *d)
{
    if (!d)
        return;
    memset(d, 0, sizeof(*d));
}

int wp_doc_add_mesh(struct wp_doc *d, struct wp_mesh *m)
{
    if (!d || !m)
        return -EINVAL;
    if (d->nmesh >= WP_DOC_MAX_MESH)
        return -ENOSPC;
    d->albedo[d->nmesh] = NULL;
    d->meshes[d->nmesh++] = m;
    return 0;
}

int wp_doc_set_albedo(struct wp_doc *d, uint32_t mesh_i, struct wp_tex *tex)
{
    if (!d || mesh_i >= d->nmesh)
        return -EINVAL;
    d->albedo[mesh_i] = tex;
    return 0;
}

int wp_doc_add_plot(struct wp_doc *d, struct wp_plot *p)
{
    if (!d || !p)
        return -EINVAL;
    if (d->nplot >= WP_DOC_MAX_PLOT)
        return -ENOSPC;
    d->plots[d->nplot++] = p;
    return 0;
}

int wp_doc_add_card(struct wp_doc *d, struct wp_rect rect, const float rgba[4])
{
    if (!d || !rgba || !wp_rect_ok(&rect))
        return -EINVAL;
    if (d->ncards >= WP_DOC_MAX_CARD)
        return -ENOSPC;
    memset(&d->cards[d->ncards], 0, sizeof(d->cards[0]));
    d->cards[d->ncards].rect = rect;
    memcpy(d->cards[d->ncards].rgba, rgba, sizeof(d->cards[0].rgba));
    d->ncards++;
    return 0;
}

int wp_doc_set_caption(struct wp_doc *d, uint32_t i, const char *s)
{
    if (!d || !s || i >= d->ncards)
        return -EINVAL;
    snprintf(d->cards[i].caption, sizeof(d->cards[i].caption), "%s", s);
    return 0;
}

int wp_doc_set_card_rect(struct wp_doc *d, uint32_t i, struct wp_rect rect)
{
    if (!d || i >= d->ncards || !wp_rect_ok(&rect))
        return -EINVAL;
    d->cards[i].rect = rect;
    return 0;
}

int wp_doc_set_card_xy(struct wp_doc *d, uint32_t i, float x, float y)
{
    if (!d || i >= d->ncards)
        return -EINVAL;
    d->cards[i].rect.x = x;
    d->cards[i].rect.y = y;
    return 0;
}

int wp_doc_apply_drag(struct wp_doc *d, uint32_t hit_id, struct wp_rect drag)
{
    uint32_t i;

    if (!d)
        return -EINVAL;
    if (hit_id == 0)
        return 0;
    i = hit_id - 1;
    if (i >= d->ncards)
        return -EINVAL;
    d->cards[i].rect.x = drag.x;
    d->cards[i].rect.y = drag.y;
    return 0;
}

struct wp_mesh *wp_doc_mesh(const struct wp_doc *d, uint32_t i)
{
    if (!d || i >= d->nmesh)
        return NULL;
    return d->meshes[i];
}

struct wp_tex *wp_doc_albedo(const struct wp_doc *d, uint32_t i)
{
    if (!d || i >= d->nmesh)
        return NULL;
    return d->albedo[i];
}

struct wp_plot *wp_doc_plot(const struct wp_doc *d, uint32_t i)
{
    if (!d || i >= d->nplot)
        return NULL;
    return d->plots[i];
}

const struct wp_doc_card *wp_doc_card(const struct wp_doc *d, uint32_t i)
{
    if (!d || i >= d->ncards)
        return NULL;
    return &d->cards[i];
}

const char *wp_doc_caption(const struct wp_doc *d, uint32_t i)
{
    if (!d || i >= d->ncards)
        return "";
    return d->cards[i].caption;
}

uint32_t wp_doc_nmesh(const struct wp_doc *d)
{
    return d ? d->nmesh : 0;
}

uint32_t wp_doc_nplot(const struct wp_doc *d)
{
    return d ? d->nplot : 0;
}

uint32_t wp_doc_ncards(const struct wp_doc *d)
{
    return d ? d->ncards : 0;
}

int wp_doc_fill_hits(const struct wp_doc *d, struct wp_hit_stack *h)
{
    uint32_t i;
    int ret;

    if (!d || !h)
        return -EINVAL;
    for (i = 0; i < d->ncards; i++) {
        ret = wp_hit_push(h, i + 1, d->cards[i].rect);
        if (ret < 0)
            return ret;
    }
    return 0;
}

int wp_doc_push_cards(const struct wp_doc *d, struct wp_draw_list *l, float scale)
{
    uint32_t i;
    int ret;

    if (!d || !l)
        return -EINVAL;
    if (scale <= 0.0f)
        scale = 1.0f;
    for (i = 0; i < d->ncards; i++) {
        struct wp_rect r = wp_rect_scaled(d->cards[i].rect, scale);
        ret = wp_draw_list_push_card(l, &r, d->cards[i].rgba);
        if (ret < 0)
            return ret;
    }
    return 0;
}

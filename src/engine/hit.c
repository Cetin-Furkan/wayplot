#define _GNU_SOURCE
#include "engine/hit.h"

#include <errno.h>

void wp_hit_clear(struct wp_hit_stack *h)
{
    if (!h)
        return;
    h->count = 0;
}

int wp_hit_push(struct wp_hit_stack *h, uint32_t id, struct wp_rect rect)
{
    if (!h || id == 0 || !wp_rect_ok(&rect))
        return -EINVAL;
    if (h->count >= WP_HIT_MAX)
        return -ENOSPC;
    h->items[h->count].id = id;
    h->items[h->count].rect = rect;
    h->count++;
    return 0;
}

static int contains(const struct wp_rect *r, float x, float y)
{
    return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h;
}

int wp_hit_pick(const struct wp_hit_stack *h, float x, float y, struct wp_hit_item *out)
{
    uint32_t i;

    if (!h || h->count == 0)
        return 0;
    i = h->count;
    while (i--) {
        if (!contains(&h->items[i].rect, x, y))
            continue;
        if (out)
            *out = h->items[i];
        return 1;
    }
    return 0;
}

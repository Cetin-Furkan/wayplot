#ifndef ENGINE_HIT_H
#define ENGINE_HIT_H

#include "renderer/card.h"

#include <stdint.h>

/*
 * CPU hit stack. Last push is on top. Rects are logical surface pixels
 * (Y down), same space as wl_pointer. Not triangles, not a widget tree.
 * See docs/HIT.md.
 */

#define WP_HIT_MAX 64

struct wp_hit_item {
    uint32_t id; /* 0 is never a hit */
    struct wp_rect rect;
};

struct wp_hit_stack {
    struct wp_hit_item items[WP_HIT_MAX];
    uint32_t count;
};

void wp_hit_clear(struct wp_hit_stack *h);

[[nodiscard]] int wp_hit_push(struct wp_hit_stack *h, uint32_t id, struct wp_rect rect);

/* 1 if a rect contains (x,y), 0 miss. Walks top-down. out may be NULL. */
int wp_hit_pick(const struct wp_hit_stack *h, float x, float y, struct wp_hit_item *out);

#endif /* ENGINE_HIT_H */

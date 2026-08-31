#define _GNU_SOURCE
#include "engine/input.h"

#include "wayland/session.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

static_assert(WP_INPUT_LEFT == WP_POINTER_LEFT, "session snapshot bit matches host");
static_assert(WP_INPUT_MIDDLE == WP_POINTER_MIDDLE, "middle bit matches");
static_assert(WP_INPUT_RIGHT == WP_POINTER_RIGHT, "right bit matches");

void wp_input_init(struct wp_input *in)
{
    if (!in)
        return;
    memset(in, 0, sizeof(*in));
}

static int in_rect(const struct wp_rect *r, float x, float y)
{
    return wp_rect_ok(r) && x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h;
}

static int pick_view(const struct wp_rect *views, uint32_t n, float x, float y)
{
    uint32_t i;

    if (!views)
        return -1;
    for (i = 0; i < n; i++) {
        if (in_rect(&views[i], x, y))
            return (int)i;
    }
    return -1;
}

void wp_input_feed(struct wp_input *in, const struct wp_pointer *p, const struct wp_hit_stack *hits,
                   const struct wp_rect *views, uint32_t nviews, int32_t logical_w,
                   int32_t logical_h, uint32_t toplevel_states)
{
    struct wp_hit_item it;
    uint32_t edges;

    if (!in || !p)
        return;
    in->chrome = WP_CHROME_NONE;
    in->resize_edges = 0;
    in->orbit_dx = 0;
    in->orbit_dy = 0;
    in->pan_dx = 0;
    in->pan_dy = 0;
    in->axis_v = 0;
    in->axis_h = 0;
    in->hover_i = -1;
    if (p->inside)
        in->hover_i = pick_view(views, nviews, (float)p->x, (float)p->y);
    if (in->hover_i >= 0) {
        in->axis_v = p->axis_v;
        in->axis_h = p->axis_h;
    }

    if (in->drag_id) {
        if (p->inside) {
            in->drag_rect.x = (float)p->x - in->grab_dx;
            in->drag_rect.y = (float)p->y - in->grab_dy;
        }
        if ((p->released & WP_INPUT_LEFT) || !p->inside)
            in->drag_id = 0;
        return;
    }

    if (in->orbiting) {
        if (p->inside) {
            in->orbit_dx = p->x - in->orbit_last_x;
            in->orbit_dy = p->y - in->orbit_last_y;
            in->orbit_last_x = p->x;
            in->orbit_last_y = p->y;
        }
        if ((p->released & WP_INPUT_LEFT) || !p->inside)
            in->orbiting = 0;
        return;
    }

    if (in->panning) {
        if (p->inside) {
            in->pan_dx = p->x - in->pan_last_x;
            in->pan_dy = p->y - in->pan_last_y;
            in->pan_last_x = p->x;
            in->pan_last_y = p->y;
        }
        if ((p->released & WP_INPUT_PAN) || !p->inside)
            in->panning = 0;
        return;
    }

    if (!p->inside)
        return;

    if (p->pressed & WP_INPUT_PAN && in->hover_i >= 0 && !(p->pressed & WP_INPUT_LEFT)) {
        in->panning = 1;
        in->view_i = in->hover_i;
        in->pan_last_x = p->x;
        in->pan_last_y = p->y;
        if (p->released & WP_INPUT_PAN)
            in->panning = 0;
        return;
    }

    if (!(p->pressed & WP_INPUT_LEFT))
        return;

    edges = 0;
    if (!(toplevel_states & (WP_TOPLEVEL_FULLSCREEN | WP_TOPLEVEL_MAXIMIZED)))
        edges = wp_pointer_edge_mask(p->x, p->y, logical_w, logical_h);
    if (edges != WP_RESIZE_NONE) {
        in->chrome = WP_CHROME_RESIZE;
        in->resize_edges = edges;
        return;
    }

    if (hits && wp_hit_pick(hits, (float)p->x, (float)p->y, &it)) {
        in->drag_id = it.id;
        in->drag_rect = it.rect;
        in->grab_dx = (float)p->x - it.rect.x;
        in->grab_dy = (float)p->y - it.rect.y;
        if (p->released & WP_INPUT_LEFT)
            in->drag_id = 0;
        return;
    }

    if (!(toplevel_states & WP_TOPLEVEL_FULLSCREEN) && p->y >= 0 && p->y < WP_WL_MOVE_BAND) {
        in->chrome = WP_CHROME_MOVE;
        return;
    }

    if (in->hover_i >= 0) {
        in->orbiting = 1;
        in->view_i = in->hover_i;
        in->orbit_last_x = p->x;
        in->orbit_last_y = p->y;
        if (p->released & WP_INPUT_LEFT)
            in->orbiting = 0;
    }
}

int wp_input_handle(struct wp_input *in, struct wp_session *s, const struct wp_hit_stack *hits,
                    const struct wp_rect *views, uint32_t nviews)
{
    struct wp_pointer p;
    uint32_t edges, shape;
    int ret = 0;

    if (!in || !s)
        return -EINVAL;
    p.x = s->pointer_x;
    p.y = s->pointer_y;
    p.inside = s->pointer_inside ? 1 : 0;
    p.buttons = s->pointer_buttons;
    p.pressed = s->pointer_pressed;
    p.released = s->pointer_released;
    p.axis_v = s->pointer_axis_v;
    p.axis_h = s->pointer_axis_h;
    wp_input_feed(in, &p, hits, views, nviews, s->width, s->height, s->toplevel_states);

    if (in->chrome == WP_CHROME_MOVE)
        ret = wp_session_toplevel_move(s);
    else if (in->chrome == WP_CHROME_RESIZE)
        ret = wp_session_toplevel_resize(s, in->resize_edges);

    edges = wp_pointer_edge_mask(s->pointer_x, s->pointer_y, s->width, s->height);
    if (s->toplevel_states & (WP_TOPLEVEL_MAXIMIZED | WP_TOPLEVEL_FULLSCREEN))
        edges = WP_RESIZE_NONE;
    if (in->drag_id || in->orbiting || in->panning ||
        wp_hit_pick(hits, (float)s->pointer_x, (float)s->pointer_y, NULL))
        shape = WP_CURSOR_DEFAULT;
    else
        shape = wp_pointer_cursor_shape(edges);
    (void)wp_session_set_cursor(s, shape);
    return ret;
}

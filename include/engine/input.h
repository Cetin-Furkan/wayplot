#ifndef ENGINE_INPUT_H
#define ENGINE_INPUT_H

#include "engine/hit.h"

#include <stdint.h>

/*
 * Host pointer. Session reports a snapshot; this file decides drag vs
 * window chrome. Wayland does not know about cards. See docs/HIT.md.
 */

#define WP_INPUT_LEFT 1u
#define WP_INPUT_MIDDLE 2u
#define WP_INPUT_RIGHT 4u
#define WP_INPUT_PAN (WP_INPUT_MIDDLE | WP_INPUT_RIGHT)

enum wp_chrome {
    WP_CHROME_NONE = 0,
    WP_CHROME_MOVE,
    WP_CHROME_RESIZE,
};

struct wp_pointer {
    int32_t x, y; /* logical surface pixels */
    int inside;
    uint32_t buttons;
    uint32_t pressed;  /* edge this pump */
    uint32_t released; /* edge this pump */
    int32_t axis_v;    /* wl_fixed, this pump */
    int32_t axis_h;
};

struct wp_input {
    uint32_t drag_id; /* 0 = none. Sticky until release/leave. */
    struct wp_rect drag_rect;
    float grab_dx, grab_dy;
    enum wp_chrome chrome; /* one-shot on the press pump */
    uint32_t resize_edges;
    int orbiting;
    int32_t orbit_last_x, orbit_last_y;
    int32_t orbit_dx, orbit_dy; /* this feed; last sample kept on release */
    int panning;
    int32_t pan_last_x, pan_last_y;
    int32_t pan_dx, pan_dy;
    int32_t axis_v;
    int32_t axis_h;
    int view_i;  /* sticky while orbiting/panning */
    int hover_i; /* view under the pointer this feed, -1 none */
};

void wp_input_init(struct wp_input *in);

/* Pure. No Wayland sends. hits and view may be NULL. view is logical pixels. */
void wp_input_feed(struct wp_input *in, const struct wp_pointer *p, const struct wp_hit_stack *hits,
                   const struct wp_rect *views, uint32_t nviews, int32_t logical_w,
                   int32_t logical_h, uint32_t toplevel_states);

struct wp_session;

/* Snapshot session pointer, feed, request move/resize if chrome, cursor. */
[[nodiscard]] int wp_input_handle(struct wp_input *in, struct wp_session *s,
                                  const struct wp_hit_stack *hits, const struct wp_rect *views,
                                  uint32_t nviews);

#endif /* ENGINE_INPUT_H */

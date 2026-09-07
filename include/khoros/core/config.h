#ifndef KHOROS_CORE_CONFIG_H
#define KHOROS_CORE_CONFIG_H

#if __STDC_VERSION__ < 202311L
#error "Khoros requires pure ISO C23 (compile with -std=c23)"
#endif

#ifdef _STDBOOL_H
#error "<stdbool.h> is strictly banned. In C23, bool, true, and false are native language keywords."
#endif

#ifdef _STDALIGN_H
#error "<stdalign.h> is strictly banned. In C23, alignas and alignof are native language keywords."
#endif

#include <stdint.h>
#include "khoros/wayland/xdg.h"

/*
 * Window chrome and default size. Not topology (rings, hugepage, pins).
 * XDG width/height 0 means "client picks" — we pick KHR_WINDOW_DEFAULT_*.
 * Non-zero configure sizes replace the buffer. Fullscreen/maximized come
 * from the compositor's configure, or from these start flags.
 */
constexpr uint32_t KHR_WINDOW_DEFAULT_W        = 960;
constexpr uint32_t KHR_WINDOW_DEFAULT_H        = 540;
constexpr uint32_t KHR_WINDOW_CHROME_TOP       = 32; /* drag bar, pixels */
constexpr uint32_t KHR_WINDOW_CHROME_EDGE      = 8;  /* edge resize strip */
constexpr uint32_t KHR_WINDOW_CHROME_CORNER    = 16; /* extra corner hit */
constexpr uint32_t KHR_WINDOW_CHROME_CLOSE     = 32; /* title-bar close square */
constexpr uint32_t KHR_WINDOW_CARD_COUNT       = 3;  /* body, bar, close */
constexpr uint32_t KHR_HIT_LIST_MAX            = 16; /* first-match chrome */
constexpr uint32_t KHR_WINDOW_MAX_W            = 4'096;
constexpr uint32_t KHR_WINDOW_MAX_H            = 4'096;
constexpr bool     KHR_WINDOW_START_MAXIMIZED  = false;
constexpr bool     KHR_WINDOW_START_FULLSCREEN = false;

constexpr uint32_t KHR_WINDOW_MIN_W =
    KHR_WINDOW_CHROME_CORNER * 2U + 64U;
constexpr uint32_t KHR_WINDOW_MIN_H =
    KHR_WINDOW_CHROME_TOP + KHR_WINDOW_CHROME_EDGE + 64U;

constexpr uint32_t KHR_WINDOW_DBLCLICK_MS = 400;
constexpr uint32_t KHR_WINDOW_DBLCLICK_PX = 6;  /* trackpad jitter */
constexpr uint32_t KHR_WINDOW_CURSOR_PX   = 24;
constexpr uint32_t KHR_WINDOW_POPUP_W     = 220;
constexpr uint32_t KHR_WINDOW_POPUP_H     = 148;

typedef enum {
    KHR_HIT_CLIENT = 0,
    KHR_HIT_POPUP,
    KHR_HIT_CLOSE,
    KHR_HIT_MOVE,
    KHR_HIT_N,
    KHR_HIT_S,
    KHR_HIT_E,
    KHR_HIT_W,
    KHR_HIT_NE,
    KHR_HIT_NW,
    KHR_HIT_SE,
    KHR_HIT_SW,
} khr_hit_t;

typedef struct {
    uint32_t  x;
    uint32_t  y;
    uint32_t  w;
    uint32_t  h;
    khr_hit_t kind;
} khr_hit_rect_t;

typedef struct {
    khr_hit_rect_t rects[KHR_HIT_LIST_MAX];
    uint32_t       count;
} khr_hit_list_t;

[[nodiscard]]
static inline uint32_t khr_window_clamp_w(uint32_t w) {
    if (w < KHR_WINDOW_MIN_W) {
        return KHR_WINDOW_MIN_W;
    }
    if (w > KHR_WINDOW_MAX_W) {
        return KHR_WINDOW_MAX_W;
    }
    return w;
}

[[nodiscard]]
static inline uint32_t khr_window_clamp_h(uint32_t h) {
    if (h < KHR_WINDOW_MIN_H) {
        return KHR_WINDOW_MIN_H;
    }
    if (h > KHR_WINDOW_MAX_H) {
        return KHR_WINDOW_MAX_H;
    }
    return h;
}

/*
 * XDG 0×0 = client picks (default, clamped to min/max).
 * Non-zero configure is the compositor's size: must match the attached
 * buffer exactly (clamping it up is a protocol error). Only cap at MAX.
 */
static inline void khr_window_buffer_size(const khr_xdg_shell_t* shell,
                                          uint32_t* out_w, uint32_t* out_h) {
    uint32_t w = KHR_WINDOW_DEFAULT_W;
    uint32_t h = KHR_WINDOW_DEFAULT_H;
    bool from_xdg = shell != nullptr && shell->width > 0 && shell->height > 0;
    if (from_xdg) {
        w = (uint32_t)shell->width;
        h = (uint32_t)shell->height;
        if (w > KHR_WINDOW_MAX_W) {
            w = KHR_WINDOW_MAX_W;
        }
        if (h > KHR_WINDOW_MAX_H) {
            h = KHR_WINDOW_MAX_H;
        }
    } else {
        w = khr_window_clamp_w(w);
        h = khr_window_clamp_h(h);
    }
    if (out_w != nullptr) {
        *out_w = w;
    }
    if (out_h != nullptr) {
        *out_h = h;
    }
}

static inline void khr_hit_list_clear(khr_hit_list_t* list) {
    if (list != nullptr) {
        list->count = 0;
    }
}

[[nodiscard]]
static inline bool khr_hit_list_add(khr_hit_list_t* list, uint32_t x, uint32_t y,
                                    uint32_t w, uint32_t h, khr_hit_t kind) {
    if (list == nullptr || w == 0 || h == 0 || list->count >= KHR_HIT_LIST_MAX) {
        return false;
    }
    list->rects[list->count] = (khr_hit_rect_t){
        .x = x,
        .y = y,
        .w = w,
        .h = h,
        .kind = kind,
    };
    list->count++;
    return true;
}

[[nodiscard]]
static inline khr_hit_t khr_hit_list_pick(const khr_hit_list_t* list,
                                          int32_t x, int32_t y) {
    if (list == nullptr || x < 0 || y < 0) {
        return KHR_HIT_CLIENT;
    }
    const uint32_t ux = (uint32_t)x;
    const uint32_t uy = (uint32_t)y;
    for (uint32_t i = 0; i < list->count; i++) {
        const khr_hit_rect_t* r = &list->rects[i];
        if (ux >= r->x && uy >= r->y &&
            ux < r->x + r->w && uy < r->y + r->h) {
            return r->kind;
        }
    }
    return KHR_HIT_CLIENT;
}

/*
 * First match wins. Corners and edges are inserted before CLOSE so the
 * outer 8–16 px stay resize (NE on the top-right extra). CLOSE sits on
 * the remaining title-bar square; MOVE is the rest of the 32 px bar.
 */
static inline void khr_window_hit_list_fill(khr_hit_list_t* list, uint32_t w,
                                            uint32_t h, bool fullscreen) {
    khr_hit_list_clear(list);
    if (list == nullptr || fullscreen || w == 0 || h == 0) {
        return;
    }
    const uint32_t c = KHR_WINDOW_CHROME_CORNER;
    const uint32_t e = KHR_WINDOW_CHROME_EDGE;
    const uint32_t t = KHR_WINDOW_CHROME_TOP;
    const uint32_t z = KHR_WINDOW_CHROME_CLOSE;
    if (w >= c && h >= c) {
        (void)khr_hit_list_add(list, 0, 0, c, c, KHR_HIT_NW);
        (void)khr_hit_list_add(list, w - c, 0, c, c, KHR_HIT_NE);
        (void)khr_hit_list_add(list, 0, h - c, c, c, KHR_HIT_SW);
        (void)khr_hit_list_add(list, w - c, h - c, c, c, KHR_HIT_SE);
    }
    if (w >= e && h >= e) {
        (void)khr_hit_list_add(list, 0, 0, e, h, KHR_HIT_W);
        (void)khr_hit_list_add(list, w - e, 0, e, h, KHR_HIT_E);
        (void)khr_hit_list_add(list, 0, h - e, w, e, KHR_HIT_S);
    }
    if (w >= z && t > 0) {
        (void)khr_hit_list_add(list, w - z, 0, z, t, KHR_HIT_CLOSE);
    }
    if (t > 0) {
        (void)khr_hit_list_add(list, 0, 0, w, t, KHR_HIT_MOVE);
    }
}

[[nodiscard]]
static inline khr_hit_t khr_window_hit(int32_t x, int32_t y, uint32_t w, uint32_t h,
                                       bool fullscreen) {
    if (fullscreen || w == 0 || h == 0 || x < 0 || y < 0 ||
        (uint32_t)x >= w || (uint32_t)y >= h) {
        return KHR_HIT_CLIENT;
    }
    khr_hit_list_t list = {};
    khr_window_hit_list_fill(&list, w, h, fullscreen);
    return khr_hit_list_pick(&list, x, y);
}

[[nodiscard]]
static inline bool khr_window_dblclick_near(int32_t x, int32_t y,
                                            uint32_t last_x, uint32_t last_y) {
    int32_t dx = x - (int32_t)last_x;
    int32_t dy = y - (int32_t)last_y;
    if (dx < 0) {
        dx = -dx;
    }
    if (dy < 0) {
        dy = -dy;
    }
    return dx <= (int32_t)KHR_WINDOW_DBLCLICK_PX &&
           dy <= (int32_t)KHR_WINDOW_DBLCLICK_PX;
}

[[nodiscard]]
static inline uint32_t khr_hit_resize_edge(khr_hit_t hit) {
    switch (hit) {
    case KHR_HIT_N:  return KHR_XDG_RESIZE_TOP;
    case KHR_HIT_S:  return KHR_XDG_RESIZE_BOTTOM;
    case KHR_HIT_E:  return KHR_XDG_RESIZE_RIGHT;
    case KHR_HIT_W:  return KHR_XDG_RESIZE_LEFT;
    case KHR_HIT_NE: return KHR_XDG_RESIZE_TOP_RIGHT;
    case KHR_HIT_NW: return KHR_XDG_RESIZE_TOP_LEFT;
    case KHR_HIT_SE: return KHR_XDG_RESIZE_BOTTOM_RIGHT;
    case KHR_HIT_SW: return KHR_XDG_RESIZE_BOTTOM_LEFT;
    default:         return KHR_XDG_RESIZE_NONE;
    }
}

#endif /* KHOROS_CORE_CONFIG_H */

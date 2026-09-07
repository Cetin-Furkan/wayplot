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

[[nodiscard]]
static inline khr_hit_t khr_window_hit(int32_t x, int32_t y, uint32_t w, uint32_t h,
                                       bool fullscreen) {
    if (fullscreen || w == 0 || h == 0 || x < 0 || y < 0 ||
        (uint32_t)x >= w || (uint32_t)y >= h) {
        return KHR_HIT_CLIENT;
    }
    const uint32_t ux = (uint32_t)x;
    const uint32_t uy = (uint32_t)y;
    const uint32_t c = KHR_WINDOW_CHROME_CORNER;
    const uint32_t e = KHR_WINDOW_CHROME_EDGE;
    const uint32_t t = KHR_WINDOW_CHROME_TOP;
    const bool near_l = ux < c;
    const bool near_r = ux >= w - c;
    const bool near_t = uy < c;
    const bool near_b = uy >= h - c;
    if (near_l && near_t) {
        return KHR_HIT_NW;
    }
    if (near_r && near_t) {
        return KHR_HIT_NE;
    }
    if (near_l && near_b) {
        return KHR_HIT_SW;
    }
    if (near_r && near_b) {
        return KHR_HIT_SE;
    }
    if (ux < e) {
        return KHR_HIT_W;
    }
    if (ux >= w - e) {
        return KHR_HIT_E;
    }
    if (uy >= h - e) {
        return KHR_HIT_S;
    }
    if (uy < t) {
        return KHR_HIT_MOVE;
    }
    return KHR_HIT_CLIENT;
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

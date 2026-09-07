#ifndef KHOROS_WAYLAND_CURSOR_H
#define KHOROS_WAYLAND_CURSOR_H

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
#include "khoros/core/attributes.h"
#include "khoros/core/config.h"
#include "khoros/wayland/client.h"
#include "khoros/wayland/shm.h"

/*
 * Mutter will not show a pointer on the client surface until we set a cursor
 * (Hyprland often shows a compositor default). Prefer wp_cursor_shape_v1;
 * fall back to a tiny wl_shm arrow + wl_pointer.set_cursor.
 */
constexpr uint16_t KHR_CURSOR_SHAPE_GET_POINTER = 1;
constexpr uint16_t KHR_CURSOR_SHAPE_SET_SHAPE   = 1;
constexpr uint32_t KHR_CURSOR_SHAPE_DEFAULT     = 1;
constexpr uint32_t KHR_CURSOR_SHAPE_POINTER     = 4;
constexpr uint32_t KHR_CURSOR_SHAPE_MOVE        = 13;
constexpr uint32_t KHR_CURSOR_SHAPE_E           = 18;
constexpr uint32_t KHR_CURSOR_SHAPE_N           = 19;
constexpr uint32_t KHR_CURSOR_SHAPE_NE          = 20;
constexpr uint32_t KHR_CURSOR_SHAPE_NW          = 21;
constexpr uint32_t KHR_CURSOR_SHAPE_S           = 22;
constexpr uint32_t KHR_CURSOR_SHAPE_SE          = 23;
constexpr uint32_t KHR_CURSOR_SHAPE_SW          = 24;
constexpr uint32_t KHR_CURSOR_SHAPE_W           = 25;

typedef struct {
    uint32_t mgr_id;
    uint32_t device_id;
    uint32_t surface_id;
    uint32_t buffer_id;
    uint32_t shm_id;
    uint32_t last_shape;
    uint32_t last_serial;
    khr_shm_pool_t pool;
    bool     shape_proto;
    bool     shm_live;
} khr_cursor_t;

void khr_cursor_init(khr_cursor_t* cur);

[[nodiscard]]
bool khr_cursor_setup(khr_wl_client_t* client, uint32_t compositor_id,
                      uint32_t pointer_id, khr_cursor_t* cur);

/* Always (re)apply on pointer.enter — Mutter needs the serial from enter. */
[[nodiscard]]
bool khr_cursor_apply(khr_wl_client_t* client, khr_cursor_t* cur,
                      uint32_t pointer_id, uint32_t serial, khr_hit_t hit,
                      bool force);

void khr_cursor_destroy(khr_wl_client_t* client, khr_cursor_t* cur);

[[nodiscard]]
static inline uint32_t khr_hit_cursor_shape(khr_hit_t hit) {
    switch (hit) {
    case KHR_HIT_MOVE: return KHR_CURSOR_SHAPE_MOVE;
    case KHR_HIT_N:    return KHR_CURSOR_SHAPE_N;
    case KHR_HIT_S:    return KHR_CURSOR_SHAPE_S;
    case KHR_HIT_E:    return KHR_CURSOR_SHAPE_E;
    case KHR_HIT_W:    return KHR_CURSOR_SHAPE_W;
    case KHR_HIT_NE:   return KHR_CURSOR_SHAPE_NE;
    case KHR_HIT_NW:   return KHR_CURSOR_SHAPE_NW;
    case KHR_HIT_SE:   return KHR_CURSOR_SHAPE_SE;
    case KHR_HIT_SW:   return KHR_CURSOR_SHAPE_SW;
    default:           return KHR_CURSOR_SHAPE_POINTER;
    }
}

#endif /* KHOROS_WAYLAND_CURSOR_H */

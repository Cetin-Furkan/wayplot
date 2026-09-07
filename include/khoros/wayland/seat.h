#ifndef KHOROS_WAYLAND_SEAT_H
#define KHOROS_WAYLAND_SEAT_H

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
#include "khoros/wayland/client.h"

constexpr uint16_t KHR_WL_SEAT_GET_POINTER   = 0;
constexpr uint16_t KHR_WL_SEAT_GET_KEYBOARD  = 1;
constexpr uint16_t KHR_WL_SEAT_EVENT_CAPS    = 0;
constexpr uint16_t KHR_WL_POINTER_SET_CURSOR = 0;
constexpr uint16_t KHR_WL_POINTER_EVENT_ENTER  = 0;
constexpr uint16_t KHR_WL_POINTER_EVENT_LEAVE  = 1;
constexpr uint16_t KHR_WL_POINTER_EVENT_MOTION = 2;
constexpr uint16_t KHR_WL_POINTER_EVENT_BUTTON = 3;
constexpr uint16_t KHR_WL_KEYBOARD_EVENT_KEY   = 3;
constexpr uint32_t KHR_WL_SEAT_CAP_POINTER   = 1;
constexpr uint32_t KHR_WL_SEAT_CAP_KEYBOARD  = 2;
constexpr uint32_t KHR_WL_POINTER_PRESSED    = 1;
constexpr uint32_t KHR_WL_POINTER_RELEASED   = 0;
constexpr uint32_t KHR_BTN_LEFT              = 0x110U;
constexpr uint32_t KHR_KEY_ESC               = 1;
constexpr uint32_t KHR_KEY_F11               = 87;
constexpr uint32_t KHR_WL_KEY_PRESSED        = 1;

typedef struct {
    uint32_t seat_id;
    uint32_t pointer_id;
    uint32_t keyboard_id;
    uint32_t caps;
    uint32_t enter_serial;
    uint32_t button_serial;
    int32_t  x;
    int32_t  y;
    uint32_t button_time_ms;
    uint32_t last_click_ms;
    uint32_t last_click_x;
    uint32_t last_click_y;
    bool     pointer_in;
    bool     left_down;
    bool     double_click;
    bool     f11_pressed;
    bool     esc_pressed;
} khr_seat_t;

void khr_seat_init(khr_seat_t* seat);

[[nodiscard]]
bool khr_seat_bind(khr_wl_client_t* client, khr_seat_t* seat);

/* After capabilities: get_pointer / get_keyboard. Safe to call again. */
[[nodiscard]]
bool khr_seat_offer_devices(khr_wl_client_t* client, khr_seat_t* seat);

uint32_t khr_seat_consume(khr_wl_client_t* client, khr_seat_t* seat,
                          const uint8_t* data, size_t len);

[[nodiscard]]
static inline uint32_t khr_seat_serial(const khr_seat_t* seat) {
    if (seat == nullptr) {
        return 0;
    }
    if (seat->button_serial != 0) {
        return seat->button_serial;
    }
    return seat->enter_serial;
}

#endif /* KHOROS_WAYLAND_SEAT_H */

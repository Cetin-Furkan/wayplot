#include "khoros/wayland/seat.h"
#include "khoros/wayland/wire.h"

#include <string.h>

void khr_seat_init(khr_seat_t* seat) {
    if (seat != nullptr) {
        *seat = (khr_seat_t){};
    }
}

[[nodiscard]]
bool khr_seat_bind(khr_wl_client_t* client, khr_seat_t* seat) {
    if (client == nullptr || seat == nullptr) {
        return false;
    }
    const khr_wl_global_t* g = khr_wl_client_find_global(client, "wl_seat");
    if (g == nullptr) {
        return true; /* optional: no pointer, no cursor, window still maps */
    }
    seat->seat_id = khr_wl_client_alloc_id(client);
    if (seat->seat_id == 0) {
        return false;
    }
    uint32_t slen = (uint32_t)strlen("wl_seat") + 1U;
    uint16_t total = (uint16_t)(24U + khr_wl_pad4(slen));
    khr_wl_msg_buf_t out = {};
    khr_wl_buf_init(&out);
    return khr_wl_encode_header(&out, KHR_WL_REGISTRY_ID, KHR_WL_REGISTRY_BIND, total) &&
           khr_wl_encode_u32(&out, g->name) &&
           khr_wl_encode_string(&out, "wl_seat") &&
           khr_wl_encode_u32(&out, g->version) &&
           khr_wl_encode_u32(&out, seat->seat_id) &&
           khr_wl_client_send_skip(client, out.data, out.size);
}

[[nodiscard]]
bool khr_seat_offer_devices(khr_wl_client_t* client, khr_seat_t* seat) {
    if (client == nullptr || seat == nullptr || seat->seat_id == 0) {
        return true;
    }
    khr_wl_msg_buf_t out = {};
    khr_wl_buf_init(&out);
    if ((seat->caps & KHR_WL_SEAT_CAP_POINTER) != 0 && seat->pointer_id == 0) {
        seat->pointer_id = khr_wl_client_alloc_id(client);
        if (seat->pointer_id == 0 ||
            !khr_wl_encode_header(&out, seat->seat_id, KHR_WL_SEAT_GET_POINTER, 12) ||
            !khr_wl_encode_u32(&out, seat->pointer_id)) {
            return false;
        }
    }
    if ((seat->caps & KHR_WL_SEAT_CAP_KEYBOARD) != 0 && seat->keyboard_id == 0) {
        seat->keyboard_id = khr_wl_client_alloc_id(client);
        if (seat->keyboard_id == 0 ||
            !khr_wl_encode_header(&out, seat->seat_id, KHR_WL_SEAT_GET_KEYBOARD, 12) ||
            !khr_wl_encode_u32(&out, seat->keyboard_id)) {
            return false;
        }
    }
    if (out.size == 0) {
        return true;
    }
    return khr_wl_client_send_skip(client, out.data, out.size);
}

[[nodiscard]]
static int32_t khr_fixed_to_px(int32_t fixed) {
    return fixed / 256;
}

uint32_t khr_seat_consume(khr_wl_client_t* client, khr_seat_t* seat,
                          const uint8_t* data, size_t len) {
    (void)client;
    if (seat == nullptr || data == nullptr || seat->seat_id == 0) {
        return 0;
    }
    seat->double_click = false;
    seat->f11_pressed = false;
    seat->esc_pressed = false;
    uint32_t count = 0;
    size_t offset = 0;
    while (offset + 8 <= len) {
        khr_wl_msg_header_t hdr = {};
        if (!khr_wl_decode_header(data + offset, len - offset, &hdr)) {
            break;
        }
        if (hdr.size < 8 || offset + hdr.size > len) {
            break;
        }
        const uint8_t* payload = data + offset + 8;
        size_t payload_len = hdr.size - 8;
        size_t off = 0;
        if (hdr.object_id == seat->seat_id &&
            hdr.opcode == KHR_WL_SEAT_EVENT_CAPS && payload_len >= 4) {
            uint32_t caps = 0;
            if (khr_wl_decode_u32(payload, payload_len, &off, &caps)) {
                seat->caps = caps;
                count++;
            }
        } else if (seat->pointer_id != 0 && hdr.object_id == seat->pointer_id) {
            if (hdr.opcode == KHR_WL_POINTER_EVENT_ENTER && payload_len >= 16) {
                uint32_t serial = 0, surf = 0;
                int32_t fx = 0, fy = 0;
                if (khr_wl_decode_u32(payload, payload_len, &off, &serial) &&
                    khr_wl_decode_u32(payload, payload_len, &off, &surf) &&
                    khr_wl_decode_i32(payload, payload_len, &off, &fx) &&
                    khr_wl_decode_i32(payload, payload_len, &off, &fy)) {
                    seat->enter_serial = serial;
                    seat->x = khr_fixed_to_px(fx);
                    seat->y = khr_fixed_to_px(fy);
                    seat->pointer_in = true;
                    count++;
                }
            } else if (hdr.opcode == KHR_WL_POINTER_EVENT_LEAVE) {
                seat->pointer_in = false;
                count++;
            } else if (hdr.opcode == KHR_WL_POINTER_EVENT_MOTION &&
                       payload_len >= 12) {
                uint32_t time = 0;
                int32_t fx = 0, fy = 0;
                if (khr_wl_decode_u32(payload, payload_len, &off, &time) &&
                    khr_wl_decode_i32(payload, payload_len, &off, &fx) &&
                    khr_wl_decode_i32(payload, payload_len, &off, &fy)) {
                    (void)time;
                    seat->x = khr_fixed_to_px(fx);
                    seat->y = khr_fixed_to_px(fy);
                    count++;
                }
            } else if (hdr.opcode == KHR_WL_POINTER_EVENT_BUTTON &&
                       payload_len >= 16) {
                uint32_t serial = 0, time = 0, button = 0, state = 0;
                if (khr_wl_decode_u32(payload, payload_len, &off, &serial) &&
                    khr_wl_decode_u32(payload, payload_len, &off, &time) &&
                    khr_wl_decode_u32(payload, payload_len, &off, &button) &&
                    khr_wl_decode_u32(payload, payload_len, &off, &state)) {
                    if (button == KHR_BTN_LEFT) {
                        seat->button_serial = serial;
                        seat->button_time_ms = time;
                        if (state == KHR_WL_POINTER_PRESSED) {
                            if (seat->left_down == false &&
                                time - seat->last_click_ms <= 400U &&
                                (uint32_t)seat->x == seat->last_click_x &&
                                (uint32_t)seat->y == seat->last_click_y) {
                                seat->double_click = true;
                            }
                            seat->last_click_ms = time;
                            seat->last_click_x = (uint32_t)seat->x;
                            seat->last_click_y = (uint32_t)seat->y;
                            seat->left_down = true;
                        } else {
                            seat->left_down = false;
                        }
                    }
                    count++;
                }
            }
        } else if (seat->keyboard_id != 0 && hdr.object_id == seat->keyboard_id &&
                   hdr.opcode == KHR_WL_KEYBOARD_EVENT_KEY && payload_len >= 16) {
            uint32_t serial = 0, time = 0, key = 0, state = 0;
            if (khr_wl_decode_u32(payload, payload_len, &off, &serial) &&
                khr_wl_decode_u32(payload, payload_len, &off, &time) &&
                khr_wl_decode_u32(payload, payload_len, &off, &key) &&
                khr_wl_decode_u32(payload, payload_len, &off, &state)) {
                (void)serial;
                (void)time;
                if (state == KHR_WL_KEY_PRESSED) {
                    if (key == KHR_KEY_F11) {
                        seat->f11_pressed = true;
                    } else if (key == KHR_KEY_ESC) {
                        seat->esc_pressed = true;
                    }
                }
                count++;
            }
        }
        offset += hdr.size;
    }
    return count;
}

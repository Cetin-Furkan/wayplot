#ifndef KHOROS_WAYLAND_WIRE_H
#define KHOROS_WAYLAND_WIRE_H

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
#include <stddef.h>
#include "khoros/core/attributes.h"

/* Fixed Core Object IDs */
constexpr uint32_t KHR_WL_DISPLAY_ID   = 1;
constexpr uint32_t KHR_WL_REGISTRY_ID  = 2;
constexpr uint32_t KHR_WL_CALLBACK_ID  = 3;

/* Core Display & Registry Opcodes */
constexpr uint16_t KHR_WL_DISPLAY_SYNC          = 0;
constexpr uint16_t KHR_WL_DISPLAY_GET_REGISTRY  = 1;
constexpr uint16_t KHR_WL_REGISTRY_BIND         = 0;

/* Server Event Opcodes */
constexpr uint16_t KHR_WL_REGISTRY_EVENT_GLOBAL        = 0;
constexpr uint16_t KHR_WL_REGISTRY_EVENT_GLOBAL_REMOVE = 1;
constexpr uint16_t KHR_WL_CALLBACK_EVENT_DONE          = 0;

typedef struct {
    uint32_t object_id;
    uint16_t opcode;
    uint16_t size;
} khr_wl_msg_header_t;

typedef struct {
    uint8_t  data[4'096];
    size_t   size;
} khr_wl_msg_buf_t;

[[nodiscard]]
static inline uint32_t khr_wl_pad4(uint32_t len) {
    return (len + 3) & ~3U;
}

void khr_wl_buf_init(khr_wl_msg_buf_t* buf);

[[nodiscard]]
bool khr_wl_encode_header(khr_wl_msg_buf_t* buf, uint32_t obj_id, uint16_t opcode, uint16_t size);

[[nodiscard]]
bool khr_wl_encode_u32(khr_wl_msg_buf_t* buf, uint32_t val);

[[nodiscard]]
bool khr_wl_encode_i32(khr_wl_msg_buf_t* buf, int32_t val);

[[nodiscard]]
bool khr_wl_encode_string(khr_wl_msg_buf_t* buf, const char* str);

[[nodiscard]]
bool khr_wl_decode_header(const uint8_t* data, size_t len, khr_wl_msg_header_t* out_hdr);

[[nodiscard]]
bool khr_wl_decode_u32(const uint8_t* data, size_t len, size_t* offset, uint32_t* out_val);

[[nodiscard]]
bool khr_wl_decode_string(const uint8_t* data, size_t len, size_t* offset,
                          const char** out_str, uint32_t* out_len);

[[nodiscard]]
static inline bool khr_wl_decode_i32(const uint8_t* data, size_t len, size_t* offset, int32_t* out_val) {
    return khr_wl_decode_u32(data, len, offset, (uint32_t*)out_val);
}

#endif /* KHOROS_WAYLAND_WIRE_H */

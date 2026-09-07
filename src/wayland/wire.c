#include "khoros/wayland/wire.h"
#include <string.h>

void khr_wl_buf_init(khr_wl_msg_buf_t* buf) {
    if (buf != nullptr) {
        buf->size = 0;
        memset(buf->data, 0, sizeof(buf->data));
    }
}

[[nodiscard]]
bool khr_wl_encode_header(khr_wl_msg_buf_t* buf, uint32_t obj_id, uint16_t opcode, uint16_t size) {
    if (buf == nullptr || buf->size + 8 > sizeof(buf->data)) {
        return false;
    }
    uint64_t packed = (uint64_t)obj_id |
                      ((uint64_t)(((uint32_t)size << 16) | (uint32_t)opcode) << 32);
    memcpy(buf->data + buf->size, &packed, 8);
    buf->size += 8;
    return true;
}

[[nodiscard]]
bool khr_wl_encode_u32(khr_wl_msg_buf_t* buf, uint32_t val) {
    if (buf == nullptr || buf->size + 4 > sizeof(buf->data)) {
        return false;
    }
    memcpy(buf->data + buf->size, &val, 4);
    buf->size += 4;
    return true;
}

[[nodiscard]]
bool khr_wl_encode_i32(khr_wl_msg_buf_t* buf, int32_t val) {
    return khr_wl_encode_u32(buf, (uint32_t)val);
}

[[nodiscard]]
bool khr_wl_encode_string(khr_wl_msg_buf_t* buf, const char* str) {
    if (buf == nullptr || str == nullptr) {
        return false;
    }
    uint32_t str_len = (uint32_t)strlen(str) + 1; /* Including NUL byte */
    uint32_t padded_len = khr_wl_pad4(str_len);

    if (buf->size + 4 + padded_len > sizeof(buf->data)) {
        return false;
    }

    /* Encode length including NUL */
    if (!khr_wl_encode_u32(buf, str_len)) {
        return false;
    }

    /* Copy string bytes and zero pad to 4 bytes boundary */
    memset(buf->data + buf->size, 0, padded_len);
    memcpy(buf->data + buf->size, str, str_len);
    buf->size += padded_len;
    return true;
}

[[nodiscard]]
bool khr_wl_decode_header(const uint8_t* data, size_t len, khr_wl_msg_header_t* out_hdr) {
    if (data == nullptr || len < 8 || out_hdr == nullptr) {
        return false;
    }
    uint64_t packed = 0;
    memcpy(&packed, data, 8);
    out_hdr->object_id = (uint32_t)packed;
    uint32_t word1 = (uint32_t)(packed >> 32);
    out_hdr->size = (uint16_t)(word1 >> 16);
    out_hdr->opcode = (uint16_t)(word1 & 0xFFFF);
    if (out_hdr->size < 8 || (out_hdr->size % 4) != 0) {
        return false;
    }
    return true;
}

[[nodiscard]]
bool khr_wl_decode_u32(const uint8_t* data, size_t len, size_t* offset, uint32_t* out_val) {
    if (data == nullptr || offset == nullptr || out_val == nullptr || *offset + 4 > len) {
        return false;
    }
    memcpy(out_val, data + *offset, 4);
    *offset += 4;
    return true;
}

[[nodiscard]]
bool khr_wl_decode_string(const uint8_t* data, size_t len, size_t* offset,
                          const char** out_str, uint32_t* out_len) {
    if (data == nullptr || offset == nullptr || out_str == nullptr || out_len == nullptr) {
        return false;
    }
    uint32_t raw_len = 0;
    if (!khr_wl_decode_u32(data, len, offset, &raw_len)) {
        return false;
    }

    if (raw_len == 0) {
        *out_str = "";
        *out_len = 0;
        return true;
    }

    if (*offset > len || (size_t)raw_len > (len - *offset)) {
        return false;
    }

    uint32_t padded_len = khr_wl_pad4(raw_len);
    if ((size_t)padded_len > (len - *offset)) {
        return false;
    }

    /* Valid Wayland wire strings MUST be NUL-terminated at raw_len - 1 */
    if (data[*offset + (size_t)raw_len - 1U] != '\0') {
        return false;
    }

    *out_str = (const char*)(data + *offset);
    *out_len = raw_len;
    *offset += padded_len;
    return true;
}

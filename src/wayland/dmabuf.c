#include "khoros/wayland/dmabuf.h"

#include <string.h>

[[nodiscard]]
bool khr_dmabuf_bind(khr_wl_client_t* client, uint32_t* out_dmabuf_id) {
    if (client == nullptr || out_dmabuf_id == nullptr) {
        return false;
    }
    const khr_wl_global_t* dmabuf = khr_wl_client_find_global(client, "zwp_linux_dmabuf_v1");
    if (dmabuf == nullptr) {
        return false;
    }
    uint32_t id = khr_wl_client_alloc_id(client);
    if (id == 0) {
        return false;
    }
    khr_wl_msg_buf_t out = {};
    khr_wl_buf_init(&out);
    const char* iface = "zwp_linux_dmabuf_v1";
    uint32_t slen = (uint32_t)strlen(iface) + 1U;
    uint16_t total = (uint16_t)(24U + khr_wl_pad4(slen));
    if (!khr_wl_encode_header(&out, KHR_WL_REGISTRY_ID, KHR_WL_REGISTRY_BIND, total) ||
        !khr_wl_encode_u32(&out, dmabuf->name) ||
        !khr_wl_encode_string(&out, iface) ||
        !khr_wl_encode_u32(&out, dmabuf->version) ||
        !khr_wl_encode_u32(&out, id)) {
        return false;
    }
    if (!khr_wl_client_send_skip(client, out.data, out.size)) {
        return false;
    }
    *out_dmabuf_id = id;
    return true;
}

[[nodiscard]]
bool khr_dmabuf_import(khr_wl_client_t* client, uint32_t dmabuf_id, int dma_fd,
                       uint32_t width, uint32_t height, uint32_t drm_format,
                       uint32_t stride, uint32_t offset, uint64_t modifier,
                       uint32_t* out_buffer_id) {
    uint32_t params_id = 0;
    return khr_dmabuf_import_full(client, dmabuf_id, dma_fd, width, height,
                                 drm_format, stride, offset, modifier,
                                 &params_id, out_buffer_id);
}

[[nodiscard]]
bool khr_dmabuf_import_full(khr_wl_client_t* client, uint32_t dmabuf_id,
                            int dma_fd, uint32_t width, uint32_t height,
                            uint32_t drm_format, uint32_t stride,
                            uint32_t offset, uint64_t modifier,
                            uint32_t* out_params_id, uint32_t* out_buffer_id) {
    if (client == nullptr || dmabuf_id == 0 || dma_fd < 0 ||
        width == 0 || height == 0 || out_params_id == nullptr ||
        out_buffer_id == nullptr) {
        return false;
    }
    uint32_t params_id = khr_wl_client_alloc_id(client);
    uint32_t buffer_id = khr_wl_client_alloc_id(client);
    if (params_id == 0 || buffer_id == 0) {
        return false;
    }

    /* 1. create_params(new_id). */
    {
        khr_wl_msg_buf_t out = {};
        khr_wl_buf_init(&out);
        if (!khr_wl_encode_header(&out, dmabuf_id, KHR_DMABUF_CREATE_PARAMS, 12) ||
            !khr_wl_encode_u32(&out, params_id) ||
            !khr_wl_client_send_skip(client, out.data, out.size)) {
            return false;
        }
    }
    /* 2. add(fd, plane 0, offset, stride, modifier_hi, modifier_lo).
     * The FD rides SCM_RIGHTS; the byte stream carries no FD slot. */
    {
        khr_wl_msg_buf_t out = {};
        khr_wl_buf_init(&out);
        if (!khr_wl_encode_header(&out, params_id, KHR_DMABUF_PARAMS_ADD, 28) ||
            !khr_wl_encode_u32(&out, 0) || /* plane_idx */
            !khr_wl_encode_u32(&out, offset) ||
            !khr_wl_encode_u32(&out, stride) ||
            !khr_wl_encode_u32(&out, (uint32_t)(modifier >> 32)) ||
            !khr_wl_encode_u32(&out, (uint32_t)(modifier & 0xFFFF'FFFFU)) ||
            !khr_wl_client_send_with_fd(client, out.data, out.size, dma_fd)) {
            return false;
        }
    }
    /* 3. create_immed(new_id buffer, w, h, format, flags). */
    {
        khr_wl_msg_buf_t out = {};
        khr_wl_buf_init(&out);
        if (!khr_wl_encode_header(&out, params_id, KHR_DMABUF_PARAMS_CREATE_IMMED, 28) ||
            !khr_wl_encode_u32(&out, buffer_id) ||
            !khr_wl_encode_u32(&out, width) ||
            !khr_wl_encode_u32(&out, height) ||
            !khr_wl_encode_u32(&out, drm_format) ||
            !khr_wl_encode_u32(&out, 0) || /* flags */
            !khr_wl_client_send_skip(client, out.data, out.size)) {
            return false;
        }
    }
    *out_params_id = params_id;
    *out_buffer_id = buffer_id;
    return true;
}

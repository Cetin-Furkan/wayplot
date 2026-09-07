#include "khoros/wayland/syncobj.h"

#include <string.h>

[[nodiscard]]
bool khr_syncobj_bind(khr_wl_client_t* client, uint32_t* out_mgr_id) {
    if (client == nullptr || out_mgr_id == nullptr) {
        return false;
    }
    const khr_wl_global_t* mgr =
        khr_wl_client_find_global(client, "wp_linux_drm_syncobj_manager_v1");
    if (mgr == nullptr) {
        return false;
    }
    uint32_t id = khr_wl_client_alloc_id(client);
    if (id == 0) {
        return false;
    }
    khr_wl_msg_buf_t out = {};
    khr_wl_buf_init(&out);
    const char* iface = "wp_linux_drm_syncobj_manager_v1";
    uint32_t slen = (uint32_t)strlen(iface) + 1U;
    uint16_t total = (uint16_t)(24U + khr_wl_pad4(slen));
    if (!khr_wl_encode_header(&out, KHR_WL_REGISTRY_ID, KHR_WL_REGISTRY_BIND, total) ||
        !khr_wl_encode_u32(&out, mgr->name) ||
        !khr_wl_encode_string(&out, iface) ||
        !khr_wl_encode_u32(&out, mgr->version) ||
        !khr_wl_encode_u32(&out, id) ||
        !khr_wl_client_send_skip(client, out.data, out.size)) {
        return false;
    }
    *out_mgr_id = id;
    return true;
}

[[nodiscard]]
bool khr_syncobj_import_timeline(khr_wl_client_t* client, uint32_t mgr_id,
                                 int syncobj_fd, uint32_t* out_timeline_id) {
    if (client == nullptr || mgr_id == 0 || syncobj_fd < 0 || out_timeline_id == nullptr) {
        return false;
    }
    uint32_t id = khr_wl_client_alloc_id(client);
    if (id == 0) {
        return false;
    }
    khr_wl_msg_buf_t out = {};
    khr_wl_buf_init(&out);
    if (!khr_wl_encode_header(&out, mgr_id, KHR_SYNCOBJ_MGR_IMPORT_TIMELINE, 12) ||
        !khr_wl_encode_u32(&out, id) ||
        !khr_wl_client_send_with_fd(client, out.data, out.size, syncobj_fd)) {
        return false;
    }
    *out_timeline_id = id;
    return true;
}

[[nodiscard]]
bool khr_syncobj_get_surface(khr_wl_client_t* client, uint32_t mgr_id,
                             uint32_t wl_surface_id, uint32_t* out_surface_id) {
    if (client == nullptr || mgr_id == 0 || wl_surface_id == 0 || out_surface_id == nullptr) {
        return false;
    }
    uint32_t id = khr_wl_client_alloc_id(client);
    if (id == 0) {
        return false;
    }
    khr_wl_msg_buf_t out = {};
    khr_wl_buf_init(&out);
    if (!khr_wl_encode_header(&out, mgr_id, KHR_SYNCOBJ_MGR_GET_SURFACE, 16) ||
        !khr_wl_encode_u32(&out, id) ||
        !khr_wl_encode_u32(&out, wl_surface_id) ||
        !khr_wl_client_send_skip(client, out.data, out.size)) {
        return false;
    }
    *out_surface_id = id;
    return true;
}

static bool khr_syncobj_encode_point(khr_wl_msg_buf_t* out, uint32_t surface_id,
                                     uint16_t opcode, uint32_t timeline, uint64_t point) {
    return khr_wl_encode_header(out, surface_id, opcode, 20) &&
           khr_wl_encode_u32(out, timeline) &&
           khr_wl_encode_u32(out, (uint32_t)(point >> 32)) &&
           khr_wl_encode_u32(out, (uint32_t)(point & 0xFFFF'FFFFU));
}

[[nodiscard]]
bool khr_syncobj_set_points(khr_wl_client_t* client, uint32_t surface_id,
                            uint32_t acquire_timeline, uint64_t acquire_point,
                            uint32_t release_timeline, uint64_t release_point) {
    if (client == nullptr || surface_id == 0 ||
        acquire_timeline == 0 || release_timeline == 0) {
        return false;
    }
    khr_wl_msg_buf_t out = {};
    khr_wl_buf_init(&out);
    if (!khr_syncobj_encode_point(&out, surface_id, KHR_SYNCOBJ_SURFACE_SET_ACQUIRE,
                                  acquire_timeline, acquire_point) ||
        !khr_syncobj_encode_point(&out, surface_id, KHR_SYNCOBJ_SURFACE_SET_RELEASE,
                                  release_timeline, release_point) ||
        !khr_wl_client_send_skip(client, out.data, out.size)) {
        return false;
    }
    return true;
}

[[nodiscard]]
bool khr_syncobj_destroy_timeline(khr_wl_client_t* client, uint32_t timeline_id) {
    if (client == nullptr || timeline_id == 0) {
        return false;
    }
    khr_wl_msg_buf_t out = {};
    khr_wl_buf_init(&out);
    if (!khr_wl_encode_header(&out, timeline_id, KHR_SYNCOBJ_TIMELINE_DESTROY, 8) ||
        !khr_wl_client_send_skip(client, out.data, out.size)) {
        return false;
    }
    return true;
}

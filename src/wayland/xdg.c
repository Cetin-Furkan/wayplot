#include "khoros/wayland/xdg.h"

#include <string.h>

void khr_xdg_init(khr_xdg_shell_t* shell) {
    if (shell != nullptr) {
        *shell = (khr_xdg_shell_t){};
    }
}

/* wl_registry.bind: header(8) + name(4) + iface string + version(4) + new_id(4) */
static bool khr_xdg_encode_bind(khr_wl_msg_buf_t* out, uint32_t name,
                                const char* iface, uint32_t version, uint32_t new_id) {
    uint32_t slen = (uint32_t)strlen(iface) + 1U;
    uint16_t total = (uint16_t)(24U + khr_wl_pad4(slen));
    return khr_wl_encode_header(out, KHR_WL_REGISTRY_ID, KHR_WL_REGISTRY_BIND, total) &&
           khr_wl_encode_u32(out, name) &&
           khr_wl_encode_string(out, iface) &&
           khr_wl_encode_u32(out, version) &&
           khr_wl_encode_u32(out, new_id);
}

[[nodiscard]]
bool khr_xdg_bind(khr_wl_client_t* client, khr_xdg_shell_t* shell) {
    if (client == nullptr || shell == nullptr) {
        return false;
    }
    const khr_wl_global_t* compositor = khr_wl_client_find_global(client, "wl_compositor");
    const khr_wl_global_t* wm_base = khr_wl_client_find_global(client, "xdg_wm_base");
    if (compositor == nullptr || wm_base == nullptr) {
        return false;
    }

    shell->compositor_id = khr_wl_client_alloc_id(client);
    shell->wm_base_id = khr_wl_client_alloc_id(client);
    if (shell->compositor_id == 0 || shell->wm_base_id == 0) {
        return false;
    }

    khr_wl_msg_buf_t out = {};
    khr_wl_buf_init(&out);
    if (!khr_xdg_encode_bind(&out, compositor->name, "wl_compositor",
                             compositor->version, shell->compositor_id) ||
        !khr_xdg_encode_bind(&out, wm_base->name, "xdg_wm_base",
                             wm_base->version, shell->wm_base_id)) {
        return false;
    }
    if (!khr_wl_client_send_skip(client, out.data, out.size)) {
        return false;
    }
    shell->state = KHR_XDG_BOUND;
    return true;
}

static bool khr_xdg_encode_title(khr_wl_msg_buf_t* out, uint32_t toplevel,
                                 uint16_t opcode, const char* str) {
    uint32_t slen = (uint32_t)strlen(str) + 1U;
    uint16_t total = (uint16_t)(12U + khr_wl_pad4(slen));
    return khr_wl_encode_header(out, toplevel, opcode, total) &&
           khr_wl_encode_string(out, str);
}

[[nodiscard]]
bool khr_xdg_create_toplevel(khr_wl_client_t* client, khr_xdg_shell_t* shell,
                             const char* title, const char* app_id) {
    if (client == nullptr || shell == nullptr || title == nullptr || app_id == nullptr) {
        return false;
    }
    if (shell->state != KHR_XDG_BOUND) {
        return false;
    }

    shell->surface_id = khr_wl_client_alloc_id(client);
    shell->xdg_surface_id = khr_wl_client_alloc_id(client);
    shell->xdg_toplevel_id = khr_wl_client_alloc_id(client);
    if (shell->surface_id == 0 || shell->xdg_surface_id == 0 || shell->xdg_toplevel_id == 0) {
        return false;
    }

    khr_wl_msg_buf_t out = {};
    khr_wl_buf_init(&out);
    /* wl_compositor.create_surface(new_id) */
    if (!khr_wl_encode_header(&out, shell->compositor_id, KHR_WL_COMPOSITOR_CREATE_SURFACE, 12) ||
        !khr_wl_encode_u32(&out, shell->surface_id)) {
        return false;
    }
    /* xdg_wm_base.get_xdg_surface(new_id, surface) */
    if (!khr_wl_encode_header(&out, shell->wm_base_id, KHR_XDG_WM_BASE_GET_XDG_SURFACE, 16) ||
        !khr_wl_encode_u32(&out, shell->xdg_surface_id) ||
        !khr_wl_encode_u32(&out, shell->surface_id)) {
        return false;
    }
    /* xdg_surface.get_toplevel(new_id) */
    if (!khr_wl_encode_header(&out, shell->xdg_surface_id, KHR_XDG_SURFACE_GET_TOPLEVEL, 12) ||
        !khr_wl_encode_u32(&out, shell->xdg_toplevel_id)) {
        return false;
    }
    if (!khr_xdg_encode_title(&out, shell->xdg_toplevel_id, KHR_XDG_TOPLEVEL_SET_TITLE, title) ||
        !khr_xdg_encode_title(&out, shell->xdg_toplevel_id, KHR_XDG_TOPLEVEL_SET_APP_ID, app_id)) {
        return false;
    }
    /* Empty first commit: no buffer. The compositor answers with configure. */
    if (!khr_wl_encode_header(&out, shell->surface_id, KHR_WL_SURFACE_COMMIT, 8)) {
        return false;
    }
    if (!khr_wl_client_send_skip(client, out.data, out.size)) {
        return false;
    }
    shell->state = KHR_XDG_TOPLEVEL;
    return true;
}

static bool khr_xdg_send_pong(khr_wl_client_t* client, const khr_xdg_shell_t* shell,
                              uint32_t serial) {
    khr_wl_msg_buf_t out = {};
    khr_wl_buf_init(&out);
    if (!khr_wl_encode_header(&out, shell->wm_base_id, KHR_XDG_WM_BASE_PONG, 12) ||
        !khr_wl_encode_u32(&out, serial)) {
        return false;
    }
    return khr_wl_client_send_skip(client, out.data, out.size);
}

static bool khr_xdg_send_ack(khr_wl_client_t* client, const khr_xdg_shell_t* shell,
                             uint32_t serial) {
    khr_wl_msg_buf_t out = {};
    khr_wl_buf_init(&out);
    if (!khr_wl_encode_header(&out, shell->xdg_surface_id, KHR_XDG_SURFACE_ACK_CONFIGURE, 12) ||
        !khr_wl_encode_u32(&out, serial)) {
        return false;
    }
    return khr_wl_client_send_skip(client, out.data, out.size);
}

[[nodiscard]]
bool khr_xdg_ack_pending(khr_wl_client_t* client, khr_xdg_shell_t* shell) {
    if (client == nullptr || shell == nullptr || shell->xdg_surface_id == 0) {
        return false;
    }
    if (shell->pending_ack_serial == 0) {
        return true;
    }
    if (!khr_xdg_send_ack(client, shell, shell->pending_ack_serial)) {
        return false;
    }
    shell->last_ack_serial = shell->pending_ack_serial;
    shell->pending_ack_serial = 0;
    return true;
}

uint32_t khr_xdg_consume(khr_wl_client_t* client, khr_xdg_shell_t* shell,
                         const uint8_t* data, size_t len) {
    if (client == nullptr || shell == nullptr || data == nullptr) {
        return 0;
    }
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

        if (shell->wm_base_id != 0 && hdr.object_id == shell->wm_base_id &&
            hdr.opcode == KHR_XDG_WM_BASE_EVENT_PING && payload_len >= 4) {
            size_t off = 0;
            uint32_t serial = 0;
            if (khr_wl_decode_u32(payload, payload_len, &off, &serial) &&
                khr_xdg_send_pong(client, shell, serial)) {
                shell->last_ping_serial = serial;
                shell->ping_count++;
                count++;
            }
        } else if (shell->xdg_surface_id != 0 && hdr.object_id == shell->xdg_surface_id &&
                   hdr.opcode == KHR_XDG_SURFACE_EVENT_CONFIGURE && payload_len >= 4) {
            size_t off = 0;
            uint32_t serial = 0;
            if (khr_wl_decode_u32(payload, payload_len, &off, &serial)) {
                /* Do not ack here. Intermediate configures during a resize
                 * storm are replaced by the latest serial; ack goes out with
                 * the matching attach. */
                shell->pending_ack_serial = serial;
                shell->configure_count++;
                shell->configured = true;
                shell->state = KHR_XDG_CONFIGURED;
                count++;
            }
        } else if (shell->xdg_toplevel_id != 0 && hdr.object_id == shell->xdg_toplevel_id &&
                   hdr.opcode == KHR_XDG_TOPLEVEL_EVENT_CONFIGURE && payload_len >= 12) {
            size_t off = 0;
            int32_t w = 0;
            int32_t h = 0;
            uint32_t arr_len = 0;
            if (khr_wl_decode_i32(payload, payload_len, &off, &w) &&
                khr_wl_decode_i32(payload, payload_len, &off, &h) &&
                khr_wl_decode_u32(payload, payload_len, &off, &arr_len) &&
                off + arr_len <= payload_len) {
                uint32_t states = 0;
                for (uint32_t i = 0; i + 4U <= arr_len; i += 4U) {
                    uint32_t st = 0;
                    size_t so = off + i;
                    if (khr_wl_decode_u32(payload, payload_len, &so, &st)) {
                        if (st == KHR_XDG_STATE_MAXIMIZED) {
                            states |= (1U << KHR_XDG_STATE_MAXIMIZED);
                        } else if (st == KHR_XDG_STATE_FULLSCREEN) {
                            states |= (1U << KHR_XDG_STATE_FULLSCREEN);
                        } else if (st == KHR_XDG_STATE_RESIZING) {
                            states |= (1U << KHR_XDG_STATE_RESIZING);
                        } else if (st == KHR_XDG_STATE_ACTIVATED) {
                            states |= (1U << KHR_XDG_STATE_ACTIVATED);
                        }
                    }
                }
                if (shell->width != w || shell->height != h ||
                    shell->states != states) {
                    shell->size_seq++;
                }
                shell->width = w;
                shell->height = h;
                shell->states = states;
                shell->maximized =
                    (states & (1U << KHR_XDG_STATE_MAXIMIZED)) != 0;
                shell->fullscreen =
                    (states & (1U << KHR_XDG_STATE_FULLSCREEN)) != 0;
                count++;
            }
        } else if (shell->xdg_toplevel_id != 0 && hdr.object_id == shell->xdg_toplevel_id &&
                   hdr.opcode == KHR_XDG_TOPLEVEL_EVENT_CLOSE) {
            shell->closed = true;
            count++;
        } else if (shell->popup_xdg_id != 0 && hdr.object_id == shell->popup_xdg_id &&
                   hdr.opcode == KHR_XDG_SURFACE_EVENT_CONFIGURE && payload_len >= 4) {
            size_t off = 0;
            uint32_t serial = 0;
            if (khr_wl_decode_u32(payload, payload_len, &off, &serial)) {
                shell->popup_ack_serial = serial;
                shell->popup_configured = true;
                count++;
            }
        } else if (shell->popup_id != 0 && hdr.object_id == shell->popup_id &&
                   hdr.opcode == KHR_XDG_POPUP_EVENT_CONFIGURE && payload_len >= 16) {
            size_t off = 0;
            int32_t x = 0, y = 0, w = 0, h = 0;
            if (khr_wl_decode_i32(payload, payload_len, &off, &x) &&
                khr_wl_decode_i32(payload, payload_len, &off, &y) &&
                khr_wl_decode_i32(payload, payload_len, &off, &w) &&
                khr_wl_decode_i32(payload, payload_len, &off, &h)) {
                shell->popup_x = x;
                shell->popup_y = y;
                if (w > 0) {
                    shell->popup_w = w;
                }
                if (h > 0) {
                    shell->popup_h = h;
                }
                count++;
            }
        } else if (shell->popup_id != 0 && hdr.object_id == shell->popup_id &&
                   hdr.opcode == KHR_XDG_POPUP_EVENT_DONE) {
            shell->popup_done = true;
            count++;
        }
        offset += hdr.size;
    }
    return count;
}

[[nodiscard]]
bool khr_xdg_request_csd(khr_wl_client_t* client, khr_xdg_shell_t* shell) {
    if (client == nullptr || shell == nullptr || shell->xdg_toplevel_id == 0) {
        return false;
    }
    const khr_wl_global_t* deco =
        khr_wl_client_find_global(client, "zxdg_decoration_manager_v1");
    if (deco == nullptr) {
        return true; /* optional */
    }
    shell->deco_mgr_id = khr_wl_client_alloc_id(client);
    shell->deco_id = khr_wl_client_alloc_id(client);
    if (shell->deco_mgr_id == 0 || shell->deco_id == 0) {
        return false;
    }
    khr_wl_msg_buf_t out = {};
    khr_wl_buf_init(&out);
    uint32_t slen = (uint32_t)strlen("zxdg_decoration_manager_v1") + 1U;
    uint16_t bind_sz = (uint16_t)(24U + khr_wl_pad4(slen));
    if (!khr_wl_encode_header(&out, KHR_WL_REGISTRY_ID, KHR_WL_REGISTRY_BIND, bind_sz) ||
        !khr_wl_encode_u32(&out, deco->name) ||
        !khr_wl_encode_string(&out, "zxdg_decoration_manager_v1") ||
        !khr_wl_encode_u32(&out, deco->version) ||
        !khr_wl_encode_u32(&out, shell->deco_mgr_id) ||
        !khr_wl_encode_header(&out, shell->deco_mgr_id, KHR_XDG_DECO_MGR_GET_TOPLEVEL, 16) ||
        !khr_wl_encode_u32(&out, shell->deco_id) ||
        !khr_wl_encode_u32(&out, shell->xdg_toplevel_id) ||
        !khr_wl_encode_header(&out, shell->deco_id, KHR_XDG_DECO_SET_MODE, 12) ||
        !khr_wl_encode_u32(&out, KHR_XDG_DECO_CLIENT_SIDE) ||
        !khr_wl_encode_header(&out, shell->surface_id, KHR_WL_SURFACE_COMMIT, 8)) {
        return false;
    }
    return khr_wl_client_send_skip(client, out.data, out.size);
}

[[nodiscard]]
bool khr_xdg_set_min_size(khr_wl_client_t* client, const khr_xdg_shell_t* shell,
                          int32_t w, int32_t h) {
    if (client == nullptr || shell == nullptr || shell->xdg_toplevel_id == 0) {
        return false;
    }
    khr_wl_msg_buf_t out = {};
    khr_wl_buf_init(&out);
    return khr_wl_encode_header(&out, shell->xdg_toplevel_id,
                                KHR_XDG_TOPLEVEL_SET_MIN_SIZE, 16) &&
           khr_wl_encode_i32(&out, w) &&
           khr_wl_encode_i32(&out, h) &&
           khr_wl_client_send_skip(client, out.data, out.size);
}

[[nodiscard]]
bool khr_xdg_set_window_geometry(khr_wl_client_t* client,
                                 const khr_xdg_shell_t* shell,
                                 int32_t x, int32_t y, int32_t w, int32_t h) {
    if (client == nullptr || shell == nullptr || shell->xdg_surface_id == 0) {
        return false;
    }
    khr_wl_msg_buf_t out = {};
    khr_wl_buf_init(&out);
    return khr_wl_encode_header(&out, shell->xdg_surface_id,
                                KHR_XDG_SURFACE_SET_WINDOW_GEOMETRY, 24) &&
           khr_wl_encode_i32(&out, x) &&
           khr_wl_encode_i32(&out, y) &&
           khr_wl_encode_i32(&out, w) &&
           khr_wl_encode_i32(&out, h) &&
           khr_wl_client_send_skip(client, out.data, out.size);
}

[[nodiscard]]
bool khr_xdg_move(khr_wl_client_t* client, const khr_xdg_shell_t* shell,
                  uint32_t seat_id, uint32_t serial) {
    if (client == nullptr || shell == nullptr || shell->xdg_toplevel_id == 0 ||
        seat_id == 0) {
        return false;
    }
    khr_wl_msg_buf_t out = {};
    khr_wl_buf_init(&out);
    return khr_wl_encode_header(&out, shell->xdg_toplevel_id,
                                KHR_XDG_TOPLEVEL_MOVE, 16) &&
           khr_wl_encode_u32(&out, seat_id) &&
           khr_wl_encode_u32(&out, serial) &&
           khr_wl_client_send_skip(client, out.data, out.size);
}

[[nodiscard]]
bool khr_xdg_resize(khr_wl_client_t* client, const khr_xdg_shell_t* shell,
                    uint32_t seat_id, uint32_t serial, uint32_t edges) {
    if (client == nullptr || shell == nullptr || shell->xdg_toplevel_id == 0 ||
        seat_id == 0 || edges == KHR_XDG_RESIZE_NONE) {
        return false;
    }
    khr_wl_msg_buf_t out = {};
    khr_wl_buf_init(&out);
    return khr_wl_encode_header(&out, shell->xdg_toplevel_id,
                                KHR_XDG_TOPLEVEL_RESIZE, 20) &&
           khr_wl_encode_u32(&out, seat_id) &&
           khr_wl_encode_u32(&out, serial) &&
           khr_wl_encode_u32(&out, edges) &&
           khr_wl_client_send_skip(client, out.data, out.size);
}

[[nodiscard]]
bool khr_xdg_set_maximized(khr_wl_client_t* client, const khr_xdg_shell_t* shell,
                           bool on) {
    if (client == nullptr || shell == nullptr || shell->xdg_toplevel_id == 0) {
        return false;
    }
    uint16_t op = on ? KHR_XDG_TOPLEVEL_SET_MAXIMIZED
                     : KHR_XDG_TOPLEVEL_UNSET_MAXIMIZED;
    khr_wl_msg_buf_t out = {};
    khr_wl_buf_init(&out);
    return khr_wl_encode_header(&out, shell->xdg_toplevel_id, op, 8) &&
           khr_wl_client_send_skip(client, out.data, out.size);
}

[[nodiscard]]
bool khr_xdg_set_fullscreen(khr_wl_client_t* client, const khr_xdg_shell_t* shell,
                            bool on) {
    if (client == nullptr || shell == nullptr || shell->xdg_toplevel_id == 0) {
        return false;
    }
    khr_wl_msg_buf_t out = {};
    khr_wl_buf_init(&out);
    if (on) {
        return khr_wl_encode_header(&out, shell->xdg_toplevel_id,
                                    KHR_XDG_TOPLEVEL_SET_FULLSCREEN, 12) &&
               khr_wl_encode_u32(&out, 0) &&
               khr_wl_client_send_skip(client, out.data, out.size);
    }
    return khr_wl_encode_header(&out, shell->xdg_toplevel_id,
                                KHR_XDG_TOPLEVEL_UNSET_FULLSCREEN, 8) &&
           khr_wl_client_send_skip(client, out.data, out.size);
}

void khr_xdg_popup_destroy(khr_wl_client_t* client, khr_xdg_shell_t* shell) {
    if (shell == nullptr) {
        return;
    }
    if (client != nullptr && shell->popup_live) {
        khr_wl_msg_buf_t out = {};
        khr_wl_buf_init(&out);
        if (shell->popup_id != 0) {
            (void)khr_wl_encode_header(&out, shell->popup_id, KHR_XDG_POPUP_DESTROY, 8);
        }
        if (shell->popup_xdg_id != 0) {
            (void)khr_wl_encode_header(&out, shell->popup_xdg_id, KHR_XDG_SURFACE_DESTROY, 8);
        }
        if (shell->popup_surface_id != 0) {
            (void)khr_wl_encode_header(&out, shell->popup_surface_id, KHR_WL_SURFACE_DESTROY, 8);
        }
        if (out.size > 0) {
            (void)khr_wl_client_send_skip(client, out.data, out.size);
        }
    }
    shell->popup_surface_id = 0;
    shell->popup_xdg_id = 0;
    shell->popup_id = 0;
    shell->popup_ack_serial = 0;
    shell->popup_x = 0;
    shell->popup_y = 0;
    shell->popup_w = 0;
    shell->popup_h = 0;
    shell->popup_live = false;
    shell->popup_configured = false;
    shell->popup_mapped = false;
    shell->popup_done = false;
}

[[nodiscard]]
bool khr_xdg_popup_ack(khr_wl_client_t* client, khr_xdg_shell_t* shell) {
    if (client == nullptr || shell == nullptr || shell->popup_xdg_id == 0) {
        return false;
    }
    if (shell->popup_ack_serial == 0) {
        return true;
    }
    khr_wl_msg_buf_t out = {};
    khr_wl_buf_init(&out);
    if (!khr_wl_encode_header(&out, shell->popup_xdg_id, KHR_XDG_SURFACE_ACK_CONFIGURE, 12) ||
        !khr_wl_encode_u32(&out, shell->popup_ack_serial) ||
        !khr_wl_client_send_skip(client, out.data, out.size)) {
        return false;
    }
    shell->popup_ack_serial = 0;
    return true;
}

[[nodiscard]]
bool khr_xdg_popup_open(khr_wl_client_t* client, khr_xdg_shell_t* shell,
                        uint32_t seat_id, uint32_t serial,
                        int32_t anchor_x, int32_t anchor_y,
                        int32_t w, int32_t h) {
    if (client == nullptr || shell == nullptr || shell->compositor_id == 0 ||
        shell->wm_base_id == 0 || shell->xdg_surface_id == 0 || w <= 0 || h <= 0) {
        return false;
    }
    if (shell->popup_live) {
        khr_xdg_popup_destroy(client, shell);
    }
    uint32_t surf = khr_wl_client_alloc_id(client);
    uint32_t pos = khr_wl_client_alloc_id(client);
    uint32_t xdg = khr_wl_client_alloc_id(client);
    uint32_t pop = khr_wl_client_alloc_id(client);
    if (surf == 0 || pos == 0 || xdg == 0 || pop == 0) {
        return false;
    }
    uint32_t adj = KHR_XDG_CONSTRAINT_SLIDE_X | KHR_XDG_CONSTRAINT_SLIDE_Y |
                   KHR_XDG_CONSTRAINT_FLIP_X | KHR_XDG_CONSTRAINT_FLIP_Y;
    khr_wl_msg_buf_t out = {};
    khr_wl_buf_init(&out);
    if (!khr_wl_encode_header(&out, shell->compositor_id, KHR_WL_COMPOSITOR_CREATE_SURFACE, 12) ||
        !khr_wl_encode_u32(&out, surf) ||
        !khr_wl_encode_header(&out, shell->wm_base_id, KHR_XDG_WM_BASE_CREATE_POSITIONER, 12) ||
        !khr_wl_encode_u32(&out, pos) ||
        !khr_wl_encode_header(&out, pos, KHR_XDG_POS_SET_SIZE, 16) ||
        !khr_wl_encode_i32(&out, w) ||
        !khr_wl_encode_i32(&out, h) ||
        !khr_wl_encode_header(&out, pos, KHR_XDG_POS_SET_ANCHOR_RECT, 24) ||
        !khr_wl_encode_i32(&out, anchor_x) ||
        !khr_wl_encode_i32(&out, anchor_y) ||
        !khr_wl_encode_i32(&out, 1) ||
        !khr_wl_encode_i32(&out, 1) ||
        !khr_wl_encode_header(&out, pos, KHR_XDG_POS_SET_ANCHOR, 12) ||
        !khr_wl_encode_u32(&out, KHR_XDG_ANCHOR_TOP_LEFT) ||
        !khr_wl_encode_header(&out, pos, KHR_XDG_POS_SET_GRAVITY, 12) ||
        !khr_wl_encode_u32(&out, KHR_XDG_GRAVITY_BOTTOM_RIGHT) ||
        !khr_wl_encode_header(&out, pos, KHR_XDG_POS_SET_CONSTRAINT, 12) ||
        !khr_wl_encode_u32(&out, adj) ||
        !khr_wl_encode_header(&out, shell->wm_base_id, KHR_XDG_WM_BASE_GET_XDG_SURFACE, 16) ||
        !khr_wl_encode_u32(&out, xdg) ||
        !khr_wl_encode_u32(&out, surf) ||
        !khr_wl_encode_header(&out, xdg, KHR_XDG_SURFACE_GET_POPUP, 20) ||
        !khr_wl_encode_u32(&out, pop) ||
        !khr_wl_encode_u32(&out, shell->xdg_surface_id) ||
        !khr_wl_encode_u32(&out, pos) ||
        !khr_wl_encode_header(&out, xdg, KHR_XDG_SURFACE_SET_WINDOW_GEOMETRY, 24) ||
        !khr_wl_encode_i32(&out, 0) ||
        !khr_wl_encode_i32(&out, 0) ||
        !khr_wl_encode_i32(&out, w) ||
        !khr_wl_encode_i32(&out, h)) {
        return false;
    }
    if (seat_id != 0 && serial != 0) {
        if (!khr_wl_encode_header(&out, pop, KHR_XDG_POPUP_GRAB, 16) ||
            !khr_wl_encode_u32(&out, seat_id) ||
            !khr_wl_encode_u32(&out, serial)) {
            return false;
        }
    }
    if (!khr_wl_encode_header(&out, pos, KHR_XDG_POS_DESTROY, 8) ||
        !khr_wl_encode_header(&out, surf, KHR_WL_SURFACE_COMMIT, 8) ||
        !khr_wl_client_send_skip(client, out.data, out.size)) {
        return false;
    }
    shell->popup_surface_id = surf;
    shell->popup_xdg_id = xdg;
    shell->popup_id = pop;
    shell->popup_ack_serial = 0;
    shell->popup_x = anchor_x;
    shell->popup_y = anchor_y;
    shell->popup_w = w;
    shell->popup_h = h;
    shell->popup_live = true;
    shell->popup_configured = false;
    shell->popup_mapped = false;
    shell->popup_done = false;
    return true;
}

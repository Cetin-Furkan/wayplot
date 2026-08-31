#define _GNU_SOURCE
#include "wayland/session.h"
#include "wayland/wire.h"

#include "helper/log.h"

#include <errno.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint32_t cap_ver(uint32_t have, uint32_t want)
{
    return have < want ? have : want;
}

static int send_u32s(struct wp_wl_conn *c, const uint32_t *w, size_t nwords)
{
    return wp_wl_send(c, w, nwords * 4, NULL, 0);
}

static int req_op(const struct wp_proto *p, const char *iface, const char *name,
                  const struct wp_proto_msg **op)
{
    *op = wp_proto_request(p, iface, name);
    return *op ? 0 : -ENOENT;
}

uint32_t wp_xdg_states_mask(const uint32_t *states, uint32_t nbytes)
{
    uint32_t n, i, m = 0;

    if (!states || nbytes < 4)
        return 0;
    n = nbytes / 4;
    for (i = 0; i < n; i++) {
        switch (states[i]) {
        case 1:
            m |= WP_TOPLEVEL_MAXIMIZED;
            break;
        case 2:
            m |= WP_TOPLEVEL_FULLSCREEN;
            break;
        case 3:
            m |= WP_TOPLEVEL_RESIZING;
            break;
        case 4:
            m |= WP_TOPLEVEL_ACTIVATED;
            break;
        case 5:
        case 6:
        case 7:
        case 8:
            m |= WP_TOPLEVEL_TILED;
            break;
        default:
            break;
        }
    }
    return m;
}

void wp_session_buffer_size(const struct wp_session *s, uint32_t *w, uint32_t *h)
{
    int32_t sc, lw, lh;
    uint32_t bw, bh;

    sc = (s && s->scale > 0) ? s->scale : 1;
    lw = (s && s->width > 0) ? s->width : WP_WL_DEFAULT_WIDTH;
    lh = (s && s->height > 0) ? s->height : WP_WL_DEFAULT_HEIGHT;
    bw = (uint32_t)lw * (uint32_t)sc;
    bh = (uint32_t)lh * (uint32_t)sc;
    if (bw < 1)
        bw = 1;
    if (bh < 1)
        bh = 1;
    if (bw > 8192)
        bw = 8192;
    if (bh > 8192)
        bh = 8192;
    if (w)
        *w = bw;
    if (h)
        *h = bh;
}

static int32_t pick_scale(const struct wp_session *s)
{
    int32_t sc = 1;
    uint32_t i, nenter = 0;

    if (s->preferred_scale > 0)
        return s->preferred_scale;
    for (i = 0; i < s->noutputs; i++) {
        if (!s->output_on[i])
            continue;
        nenter++;
        if (s->output_scale[i] > sc)
            sc = s->output_scale[i];
    }
    if (nenter)
        return sc;
    for (i = 0; i < s->noutputs; i++) {
        if (s->output_scale[i] > sc)
            sc = s->output_scale[i];
    }
    return sc < 1 ? 1 : sc;
}

static void refresh_buffer(struct wp_session *s)
{
    uint32_t bw, bh;
    int32_t sc = pick_scale(s);

    if (sc != s->scale) {
        s->scale = sc;
        if (s->configured)
            s->size_dirty = true;
    }
    wp_session_buffer_size(s, &bw, &bh);
    if (bw != s->buf_w || bh != s->buf_h) {
        s->buf_w = bw;
        s->buf_h = bh;
        if (s->configured)
            s->size_dirty = true;
    }
}

static int bind_global(struct wp_session *s, const struct wp_global *g, uint32_t want_ver,
                       enum wp_obj_kind kind, uint32_t *out)
{
    const struct wp_proto_msg *bind;
    uint32_t b[80];
    size_t pos;
    uint32_t ver, id;
    int ret;

    if (!g)
        return -ENOENT;
    if (req_op(&s->proto, "wl_registry", "bind", &bind) < 0)
        return -ENOENT;
    ver = cap_ver(g->version, want_ver);
    id = wp_wl_alloc_id(&s->conn);
    ret = wp_map_set(&s->conn.map, id, kind, ver);
    if (ret < 0)
        return ret;
    b[0] = s->reg.registry_id;
    b[2] = g->name;
    pos = 3 + wp_wl_put_str(&b[3], g->iface);
    b[pos++] = ver;
    b[pos++] = id;
    b[1] = (uint32_t)((pos * 4) << 16) | bind->opcode;
    ret = wp_wl_send(&s->conn, b, pos * 4, NULL, 0);
    if (ret < 0)
        return ret;
    *out = id;
    return 0;
}

static int bind_iface(struct wp_session *s, const char *iface, uint32_t want_ver,
                      enum wp_obj_kind kind, uint32_t *out)
{
    return bind_global(s, wp_registry_find(&s->reg, iface), want_ver, kind, out);
}

static int output_index(const struct wp_session *s, uint32_t id)
{
    uint32_t i;
    for (i = 0; i < s->noutputs; i++) {
        if (s->outputs[i] == id)
            return (int)i;
    }
    return -1;
}

uint32_t wp_pointer_edge_mask(int32_t x, int32_t y, int32_t w, int32_t h)
{
    uint32_t e = WP_RESIZE_NONE;
    const int32_t m = WP_WL_RESIZE_MARGIN;

    if (w < 1 || h < 1)
        return WP_RESIZE_NONE;
    if (x < m)
        e |= WP_RESIZE_LEFT;
    if (x >= w - m)
        e |= WP_RESIZE_RIGHT;
    if (y < m)
        e |= WP_RESIZE_TOP;
    if (y >= h - m)
        e |= WP_RESIZE_BOTTOM;
    return e;
}

uint32_t wp_pointer_cursor_shape(uint32_t edges)
{
    switch (edges) {
    case WP_RESIZE_TOP:
        return WP_CURSOR_N_RESIZE;
    case WP_RESIZE_BOTTOM:
        return WP_CURSOR_S_RESIZE;
    case WP_RESIZE_LEFT:
        return WP_CURSOR_W_RESIZE;
    case WP_RESIZE_RIGHT:
        return WP_CURSOR_E_RESIZE;
    case WP_RESIZE_TOP_LEFT:
        return WP_CURSOR_NW_RESIZE;
    case WP_RESIZE_TOP_RIGHT:
        return WP_CURSOR_NE_RESIZE;
    case WP_RESIZE_BOTTOM_LEFT:
        return WP_CURSOR_SW_RESIZE;
    case WP_RESIZE_BOTTOM_RIGHT:
        return WP_CURSOR_SE_RESIZE;
    default:
        return WP_CURSOR_DEFAULT;
    }
}

int wp_session_set_cursor(struct wp_session *s, uint32_t shape)
{
    const struct wp_proto_msg *op;
    uint32_t msg[4];

    if (!s || !s->cursor_shape_dev || s->pointer_enter_serial == 0)
        return -EINVAL;
    if (shape == 0)
        shape = WP_CURSOR_DEFAULT;
    if (s->cursor_shape == shape)
        return 0;
    if (req_op(&s->proto, "wp_cursor_shape_device_v1", "set_shape", &op) < 0)
        return -ENOENT;
    msg[0] = s->cursor_shape_dev;
    msg[1] = (16u << 16) | op->opcode;
    msg[2] = s->pointer_enter_serial;
    msg[3] = shape;
    s->cursor_shape = shape;
    return send_u32s(&s->conn, msg, 4);
}

static int attach_cursor_shape(struct wp_session *s)
{
    const struct wp_proto_msg *op;
    uint32_t msg[4];
    uint32_t id;
    int ret;

    if (!s || !s->cursor_shape_mgr || !s->pointer || s->cursor_shape_dev)
        return 0;
    if (req_op(&s->proto, "wp_cursor_shape_manager_v1", "get_pointer", &op) < 0)
        return -ENOENT;
    id = wp_wl_alloc_id(&s->conn);
    ret = wp_map_set(&s->conn.map, id, WP_OBJ_CURSOR_SHAPE_DEV, 1);
    if (ret < 0)
        return ret;
    msg[0] = s->cursor_shape_mgr;
    msg[1] = (16u << 16) | op->opcode;
    msg[2] = id;
    msg[3] = s->pointer;
    ret = send_u32s(&s->conn, msg, 4);
    if (ret < 0)
        return ret;
    s->cursor_shape_dev = id;
    return 0;
}

static void pointer_apply_cursor(struct wp_session *s)
{
    uint32_t edges, shape;

    if (!s->pointer_inside)
        return;
    edges = wp_pointer_edge_mask(s->pointer_x, s->pointer_y, s->width, s->height);
    if ((s->toplevel_states & (WP_TOPLEVEL_MAXIMIZED | WP_TOPLEVEL_FULLSCREEN)) != 0)
        edges = WP_RESIZE_NONE;
    shape = wp_pointer_cursor_shape(edges);
    (void)wp_session_set_cursor(s, shape);
}

static int pointer_button(struct wp_session *s, uint32_t button, uint32_t state)
{
    uint32_t bit;

    /* Snapshot only. Move/resize is host chrome (docs/HIT.md). */
    if (button == BTN_LEFT)
        bit = WP_POINTER_LEFT;
    else if (button == BTN_MIDDLE)
        bit = WP_POINTER_MIDDLE;
    else if (button == BTN_RIGHT)
        bit = WP_POINTER_RIGHT;
    else
        return 0;
    if (state == 1u) {
        s->pointer_buttons |= bit;
        s->pointer_pressed |= bit;
    } else {
        s->pointer_buttons &= ~bit;
        s->pointer_released |= bit;
    }
    return 0;
}

static int on_feedback(struct wp_session *s, const struct wp_wl_msg *m)
{
    const uint32_t *raw = (const uint32_t *)m->raw;
    const struct wp_proto_msg *ev;

    ev = wp_proto_event(&s->proto, "zwp_linux_dmabuf_feedback_v1", "done");
    if (ev && m->opcode == ev->opcode) {
        s->fb.done = true;
        return 0;
    }
    ev = wp_proto_event(&s->proto, "zwp_linux_dmabuf_feedback_v1", "format_table");
    if (ev && m->opcode == ev->opcode) {
        int fd = wp_wl_take_fd(&s->conn);
        uint32_t bytes = m->size >= 12 ? raw[2] : 0;
        return wp_feedback_set_table(&s->fb, fd, bytes);
    }
    ev = wp_proto_event(&s->proto, "zwp_linux_dmabuf_feedback_v1", "main_device");
    if (ev && m->opcode == ev->opcode) {
        const void *arr = NULL;
        uint32_t n = 0, next = 0;
        if (wp_wl_array_at(raw, m->size, 2, &arr, &n, &next) == 0 && n >= sizeof(dev_t)) {
            dev_t d = 0;
            memcpy(&d, arr, sizeof(dev_t));
            wp_feedback_set_main_device(&s->fb, d);
        }
        return 0;
    }
    ev = wp_proto_event(&s->proto, "zwp_linux_dmabuf_feedback_v1", "tranche_done");
    if (ev && m->opcode == ev->opcode) {
        wp_feedback_tranche_done(&s->fb);
        return 0;
    }
    ev = wp_proto_event(&s->proto, "zwp_linux_dmabuf_feedback_v1", "tranche_target_device");
    if (ev && m->opcode == ev->opcode) {
        const void *arr = NULL;
        uint32_t n = 0, next = 0;
        struct wp_tranche *tr = wp_feedback_cur(&s->fb);
        if (!tr)
            return -ENOMEM;
        if (wp_wl_array_at(raw, m->size, 2, &arr, &n, &next) == 0 && n >= sizeof(dev_t))
            memcpy(&tr->target_device, arr, sizeof(dev_t));
        return 0;
    }
    ev = wp_proto_event(&s->proto, "zwp_linux_dmabuf_feedback_v1", "tranche_formats");
    if (ev && m->opcode == ev->opcode) {
        const void *arr = NULL;
        uint32_t n = 0, next = 0;
        if (wp_wl_array_at(raw, m->size, 2, &arr, &n, &next) < 0)
            return -EBADMSG;
        if (n % 2)
            return -EBADMSG;
        return wp_feedback_add_indices(&s->fb, (const uint16_t *)arr, n / 2);
    }
    ev = wp_proto_event(&s->proto, "zwp_linux_dmabuf_feedback_v1", "tranche_flags");
    if (ev && m->opcode == ev->opcode) {
        struct wp_tranche *tr = wp_feedback_cur(&s->fb);
        if (!tr)
            return -ENOMEM;
        if (m->size >= 12)
            tr->flags = raw[2];
        return 0;
    }
    return 0;
}

int wp_session_dispatch(struct wp_session *s, const struct wp_wl_msg *m)
{
    const struct wp_obj *obj;
    const uint32_t *raw;
    const struct wp_proto_msg *ev;
    int ret;

    if (!s || !m)
        return -EINVAL;
    ret = wp_registry_handle(&s->reg, m);
    if (ret < 0)
        return ret;

    obj = wp_map_get(&s->conn.map, m->obj);
    if (obj && obj->kind == WP_OBJ_CALLBACK && m->obj == s->frame_cb) {
        s->frame_done = true;
        s->frame_cb = 0;
        return 0;
    }
    raw = (const uint32_t *)m->raw;
    if (!obj)
        return 0;

    if (obj->kind == WP_OBJ_XDG_WM) {
        ev = wp_proto_event(&s->proto, "xdg_wm_base", "ping");
        if (ev && m->opcode == ev->opcode && m->size >= 12) {
            const struct wp_proto_msg *pong;
            uint32_t msg[3];
            if (req_op(&s->proto, "xdg_wm_base", "pong", &pong) < 0)
                return -ENOENT;
            msg[0] = s->xdg_wm;
            msg[1] = (12u << 16) | pong->opcode;
            msg[2] = raw[2];
            return send_u32s(&s->conn, msg, 3);
        }
        return 0;
    }
    if (obj->kind == WP_OBJ_XDG_SURFACE) {
        ev = wp_proto_event(&s->proto, "xdg_surface", "configure");
        if (ev && m->opcode == ev->opcode && m->size >= 12) {
            s->configure_serial = raw[2];
            s->configure_dirty = true;
        }
        return 0;
    }
    if (obj->kind == WP_OBJ_XDG_TOPLEVEL) {
        ev = wp_proto_event(&s->proto, "xdg_toplevel", "configure");
        if (ev && m->opcode == ev->opcode && m->size >= 16) {
            int32_t w = (int32_t)raw[2], h = (int32_t)raw[3];
            const void *arr = NULL;
            uint32_t nbytes = 0, next = 0;
            if (wp_wl_array_at(raw, m->size, 4, &arr, &nbytes, &next) == 0)
                s->toplevel_states = wp_xdg_states_mask((const uint32_t *)arr, nbytes);
            if (w > 0 && h > 0) {
                if (s->configured && (w != s->width || h != s->height))
                    s->size_dirty = true;
                s->width = w;
                s->height = h;
                refresh_buffer(s);
            }
        }
        ev = wp_proto_event(&s->proto, "xdg_toplevel", "close");
        if (ev && m->opcode == ev->opcode)
            s->closed = true;
        return 0;
    }
    if (obj->kind == WP_OBJ_SURFACE) {
        ev = wp_proto_event(&s->proto, "wl_surface", "preferred_buffer_scale");
        if (ev && m->opcode == ev->opcode && m->size >= 12) {
            int32_t f = (int32_t)raw[2];
            s->preferred_scale = f > 0 ? f : 1;
            refresh_buffer(s);
            return 0;
        }
        ev = wp_proto_event(&s->proto, "wl_surface", "enter");
        if (ev && m->opcode == ev->opcode && m->size >= 12) {
            int ix = output_index(s, raw[2]);
            if (ix >= 0)
                s->output_on[ix] = 1;
            refresh_buffer(s);
            return 0;
        }
        ev = wp_proto_event(&s->proto, "wl_surface", "leave");
        if (ev && m->opcode == ev->opcode && m->size >= 12) {
            int ix = output_index(s, raw[2]);
            if (ix >= 0)
                s->output_on[ix] = 0;
            refresh_buffer(s);
            return 0;
        }
        return 0;
    }
    if (obj->kind == WP_OBJ_OUTPUT) {
        ev = wp_proto_event(&s->proto, "wl_output", "scale");
        if (ev && m->opcode == ev->opcode && m->size >= 12) {
            int ix = output_index(s, m->obj);
            int32_t sc = (int32_t)raw[2];
            if (ix >= 0)
                s->output_scale[ix] = sc > 0 ? sc : 1;
            refresh_buffer(s);
        }
        return 0;
    }
    if (obj->kind == WP_OBJ_SEAT) {
        ev = wp_proto_event(&s->proto, "wl_seat", "capabilities");
        if (ev && m->opcode == ev->opcode && m->size >= 12 && s->pointer == 0 &&
            (raw[2] & 1u) != 0) {
            const struct wp_proto_msg *op;
            uint32_t msg[3];
            if (req_op(&s->proto, "wl_seat", "get_pointer", &op) == 0) {
                s->pointer = wp_wl_alloc_id(&s->conn);
                if (wp_map_set(&s->conn.map, s->pointer, WP_OBJ_POINTER, 1) == 0) {
                    msg[0] = s->seat;
                    msg[1] = (12u << 16) | op->opcode;
                    msg[2] = s->pointer;
                    (void)send_u32s(&s->conn, msg, 3);
                    (void)attach_cursor_shape(s);
                    if (s->pointer_inside)
                        pointer_apply_cursor(s);
                } else {
                    s->pointer = 0;
                }
            }
        }
        return 0;
    }
    if (obj->kind == WP_OBJ_POINTER) {
        ev = wp_proto_event(&s->proto, "wl_pointer", "enter");
        if (ev && m->opcode == ev->opcode && m->size >= 24) {
            s->pointer_serial = raw[2];
            s->pointer_enter_serial = raw[2];
            s->pointer_inside = true;
            s->pointer_x = (int32_t)raw[4] / 256;
            s->pointer_y = (int32_t)raw[5] / 256;
            s->cursor_shape = 0; /* force set_shape; serial is enter, not button */
            pointer_apply_cursor(s);
            return 0;
        }
        ev = wp_proto_event(&s->proto, "wl_pointer", "leave");
        if (ev && m->opcode == ev->opcode) {
            s->pointer_inside = false;
            s->cursor_shape = 0;
            if (m->size >= 12)
                s->pointer_serial = raw[2];
            return 0;
        }
        ev = wp_proto_event(&s->proto, "wl_pointer", "motion");
        if (ev && m->opcode == ev->opcode && m->size >= 20) {
            s->pointer_x = (int32_t)raw[3] / 256;
            s->pointer_y = (int32_t)raw[4] / 256;
            pointer_apply_cursor(s);
            return 0;
        }
        ev = wp_proto_event(&s->proto, "wl_pointer", "button");
        if (ev && m->opcode == ev->opcode && m->size >= 24) {
            s->pointer_serial = raw[2];
            return pointer_button(s, raw[4], raw[5]);
        }
        ev = wp_proto_event(&s->proto, "wl_pointer", "axis");
        if (ev && m->opcode == ev->opcode && m->size >= 20) {
            int32_t val = (int32_t)raw[4];
            if (raw[3] == 0)
                s->pointer_axis_v += val;
            else if (raw[3] == 1)
                s->pointer_axis_h += val;
            return 0;
        }
        return 0;
    }
    if (obj->kind == WP_OBJ_FEEDBACK)
        return on_feedback(s, m);
    return 0;
}

int wp_session_open(struct wp_session *s)
{
    char path[108];
    int ret;

    if (!s)
        return -EINVAL;
    memset(s, 0, sizeof(*s));
    s->conn.fd = -1;
    wp_feedback_init(&s->fb);
    ret = wp_wl_display_path(path, sizeof(path));
    if (ret < 0)
        return ret;
    ret = wp_proto_core(&s->proto);
    if (ret < 0)
        return ret;
    ret = wp_wl_conn_connect(&s->conn, path);
    if (ret < 0)
        return ret;
    return wp_registry_roundtrip(&s->reg, &s->conn, &s->proto);
}

static int send_new_id(struct wp_session *s, uint32_t obj, const struct wp_proto_msg *op,
                       uint32_t new_id, enum wp_obj_kind kind)
{
    uint32_t msg[3];
    int ret;

    ret = wp_map_set(&s->conn.map, new_id, kind, 1);
    if (ret < 0)
        return ret;
    msg[0] = obj;
    msg[1] = (12u << 16) | op->opcode;
    msg[2] = new_id;
    return send_u32s(&s->conn, msg, 3);
}

int wp_session_setup_surface(struct wp_session *s)
{
    const struct wp_proto_msg *op;
    uint32_t msg[8];
    uint64_t deadline;
    int ret;

    if (!s)
        return -EINVAL;
    ret = bind_iface(s, "wl_compositor", 6, WP_OBJ_COMPOSITOR, &s->compositor);
    if (ret < 0)
        return ret;
    ret = bind_iface(s, "xdg_wm_base", 6, WP_OBJ_XDG_WM, &s->xdg_wm);
    if (ret < 0)
        return ret;
    ret = bind_iface(s, "zwp_linux_dmabuf_v1", 4, WP_OBJ_DMABUF, &s->dmabuf);
    if (ret < 0)
        return ret;
    ret = bind_iface(s, "wp_linux_drm_syncobj_manager_v1", 1, WP_OBJ_SYNCOBJ_MGR,
                     &s->syncobj_mgr);
    if (ret < 0)
        return ret;

    if (req_op(&s->proto, "wl_compositor", "create_surface", &op) < 0)
        return -ENOENT;
    s->surface = wp_wl_alloc_id(&s->conn);
    ret = send_new_id(s, s->compositor, op, s->surface, WP_OBJ_SURFACE);
    if (ret < 0)
        return ret;

    if (req_op(&s->proto, "zwp_linux_dmabuf_v1", "get_surface_feedback", &op) < 0)
        return -ENOENT;
    s->feedback = wp_wl_alloc_id(&s->conn);
    ret = wp_map_set(&s->conn.map, s->feedback, WP_OBJ_FEEDBACK, 4);
    if (ret < 0)
        return ret;
    msg[0] = s->dmabuf;
    msg[1] = (16u << 16) | op->opcode;
    msg[2] = s->feedback;
    msg[3] = s->surface;
    ret = send_u32s(&s->conn, msg, 4);
    if (ret < 0)
        return ret;

    if (req_op(&s->proto, "xdg_wm_base", "get_xdg_surface", &op) < 0)
        return -ENOENT;
    s->xdg_surface = wp_wl_alloc_id(&s->conn);
    ret = wp_map_set(&s->conn.map, s->xdg_surface, WP_OBJ_XDG_SURFACE, 1);
    if (ret < 0)
        return ret;
    msg[0] = s->xdg_wm;
    msg[1] = (16u << 16) | op->opcode;
    msg[2] = s->xdg_surface;
    msg[3] = s->surface;
    ret = send_u32s(&s->conn, msg, 4);
    if (ret < 0)
        return ret;

    if (req_op(&s->proto, "xdg_surface", "get_toplevel", &op) < 0)
        return -ENOENT;
    s->xdg_toplevel = wp_wl_alloc_id(&s->conn);
    ret = send_new_id(s, s->xdg_surface, op, s->xdg_toplevel, WP_OBJ_XDG_TOPLEVEL);
    if (ret < 0)
        return ret;

    if (req_op(&s->proto, "xdg_toplevel", "set_title", &op) == 0) {
        uint32_t t[32];
        size_t pos;
        t[0] = s->xdg_toplevel;
        pos = 2 + wp_wl_put_str(&t[2], "wayplot");
        t[1] = (uint32_t)((pos * 4) << 16) | op->opcode;
        ret = wp_wl_send(&s->conn, t, pos * 4, NULL, 0);
        if (ret < 0)
            return ret;
    }
    if (req_op(&s->proto, "xdg_toplevel", "set_min_size", &op) == 0) {
        uint32_t t[4];
        t[0] = s->xdg_toplevel;
        t[1] = (16u << 16) | op->opcode;
        t[2] = 320;
        t[3] = 200;
        ret = send_u32s(&s->conn, t, 4);
        if (ret < 0)
            return ret;
    }

    {
        uint32_t gi;
        for (gi = 0; gi < s->reg.count && s->noutputs < WP_WL_MAX_OUTPUTS; gi++) {
            uint32_t id;
            if (strcmp(s->reg.globals[gi].iface, "wl_output") != 0)
                continue;
            ret = bind_global(s, &s->reg.globals[gi], 4, WP_OBJ_OUTPUT, &id);
            if (ret < 0)
                return ret;
            s->outputs[s->noutputs] = id;
            s->output_scale[s->noutputs] = 1;
            s->output_on[s->noutputs] = 0;
            s->noutputs++;
        }
        (void)bind_iface(s, "wl_seat", 8, WP_OBJ_SEAT, &s->seat);
        (void)bind_iface(s, "wp_cursor_shape_manager_v1", 1, WP_OBJ_CURSOR_SHAPE_MGR,
                         &s->cursor_shape_mgr);
        if (s->pointer && s->cursor_shape_mgr) {
            (void)attach_cursor_shape(s);
            if (s->pointer_inside)
                pointer_apply_cursor(s);
        }
    }

    if (req_op(&s->proto, "wl_surface", "commit", &op) < 0)
        return -ENOENT;
    msg[0] = s->surface;
    msg[1] = (8u << 16) | op->opcode;
    ret = send_u32s(&s->conn, msg, 2);
    if (ret < 0)
        return ret;

    deadline = now_ns() + 5000000000ull;
    while (!(s->configure_serial && s->fb.done)) {
        struct wp_wl_msg m;
        uint64_t now = now_ns();
        uint64_t left;
        if (now >= deadline)
            return -ETIMEDOUT;
        left = deadline - now;
        ret = wp_wl_pump_wait(&s->conn, s->conn.in_len >= 8 ? 0 : 1, left);
        if (ret == -ETIME)
            return -ETIMEDOUT;
        if (ret < 0)
            return ret;
        while (wp_wl_peek(&s->conn, &m)) {
            ret = wp_session_dispatch(s, &m);
            wp_wl_consume(&s->conn);
            if (ret < 0)
                return ret;
        }
    }

    if (s->width <= 0 || s->height <= 0) {
        wp_debug("xdg_toplevel.configure 0x0; using default %dx%d\n",
                 WP_WL_DEFAULT_WIDTH, WP_WL_DEFAULT_HEIGHT);
        s->width = WP_WL_DEFAULT_WIDTH;
        s->height = WP_WL_DEFAULT_HEIGHT;
    }
    s->scale = pick_scale(s);
    refresh_buffer(s);

    if (req_op(&s->proto, "xdg_surface", "ack_configure", &op) < 0)
        return -ENOENT;
    msg[0] = s->xdg_surface;
    msg[1] = (12u << 16) | op->opcode;
    msg[2] = s->configure_serial;
    ret = send_u32s(&s->conn, msg, 3);
    if (ret < 0)
        return ret;
    s->configured = true;
    s->configure_dirty = false;
    s->frame_done = true;
    return 0;
}

int wp_session_pump(struct wp_session *s, uint64_t timeout_ns)
{
    int ret;

    if (!s)
        return -EINVAL;
    s->pointer_pressed = 0;
    s->pointer_released = 0;
    s->pointer_axis_v = 0;
    s->pointer_axis_h = 0;
    ret = wp_wl_pump_wait(&s->conn, s->conn.in_len >= 8 ? 0 : 1, timeout_ns);
    if (ret == -ETIME)
        return 0;
    if (ret < 0)
        return ret;
    {
        struct wp_wl_msg m;
        while (wp_wl_peek(&s->conn, &m)) {
            ret = wp_session_dispatch(s, &m);
            wp_wl_consume(&s->conn);
            if (ret < 0)
                return ret;
        }
    }
    return 0;
}

int wp_session_setup_explicit_sync(struct wp_session *s, int acquire_fd)
{
    const struct wp_proto_msg *op;
    uint32_t msg[4];
    int ret;

    if (!s || s->syncobj_mgr == 0 || acquire_fd < 0)
        return -EINVAL;
    if (req_op(&s->proto, "wp_linux_drm_syncobj_manager_v1", "get_surface", &op) < 0)
        return -ENOENT;
    s->syncobj_surface = wp_wl_alloc_id(&s->conn);
    ret = wp_map_set(&s->conn.map, s->syncobj_surface, WP_OBJ_SYNCOBJ_SURFACE, 1);
    if (ret < 0)
        return ret;
    msg[0] = s->syncobj_mgr;
    msg[1] = (16u << 16) | op->opcode;
    msg[2] = s->syncobj_surface;
    msg[3] = s->surface;
    ret = send_u32s(&s->conn, msg, 4);
    if (ret < 0)
        return ret;
    return wp_session_import_timeline(s, acquire_fd, &s->acquire_timeline);
}

int wp_session_import_timeline(struct wp_session *s, int fd, uint32_t *out_id)
{
    const struct wp_proto_msg *op;
    uint32_t msg[3];
    uint32_t id;
    int ret;

    if (!s || fd < 0 || !out_id)
        return -EINVAL;
    if (req_op(&s->proto, "wp_linux_drm_syncobj_manager_v1", "import_timeline", &op) < 0)
        return -ENOENT;
    id = wp_wl_alloc_id(&s->conn);
    ret = wp_map_set(&s->conn.map, id, WP_OBJ_SYNCOBJ_TIMELINE, 1);
    if (ret < 0)
        return ret;
    msg[0] = s->syncobj_mgr;
    msg[1] = (12u << 16) | op->opcode;
    msg[2] = id;
    ret = wp_wl_send(&s->conn, msg, sizeof(msg), &fd, 1);
    if (ret < 0)
        return ret;
    *out_id = id;
    return 0;
}

int wp_session_create_wl_buffer(struct wp_session *s, const struct wp_wl_export *e, uint32_t *out_id)
{
    const struct wp_proto_msg *op;
    uint32_t params, buf, p;
    uint32_t msg[8];
    int ret, send_fd;

    if (!s || !e || !out_id || e->dma_fd < 0 || e->plane_count == 0 || e->plane_count > 4)
        return -EINVAL;
    if (req_op(&s->proto, "zwp_linux_dmabuf_v1", "create_params", &op) < 0)
        return -ENOENT;
    params = wp_wl_alloc_id(&s->conn);
    ret = wp_map_set(&s->conn.map, params, WP_OBJ_DMABUF_PARAMS, 1);
    if (ret < 0)
        return ret;
    msg[0] = s->dmabuf;
    msg[1] = (12u << 16) | op->opcode;
    msg[2] = params;
    ret = send_u32s(&s->conn, msg, 3);
    if (ret < 0)
        return ret;

    if (req_op(&s->proto, "zwp_linux_buffer_params_v1", "add", &op) < 0)
        return -ENOENT;
    for (p = 0; p < e->plane_count; p++) {
        send_fd = dup(e->dma_fd);
        if (send_fd < 0)
            return -errno;
        msg[0] = params;
        msg[1] = (28u << 16) | op->opcode;
        msg[2] = p;
        msg[3] = e->offset[p];
        msg[4] = e->stride[p];
        msg[5] = (uint32_t)(e->modifier >> 32);
        msg[6] = (uint32_t)(e->modifier & 0xffffffffu);
        ret = wp_wl_send(&s->conn, msg, 28, &send_fd, 1);
        close(send_fd);
        if (ret < 0)
            return ret;
    }

    if (req_op(&s->proto, "zwp_linux_buffer_params_v1", "create_immed", &op) < 0)
        return -ENOENT;
    buf = wp_wl_alloc_id(&s->conn);
    ret = wp_map_set(&s->conn.map, buf, WP_OBJ_BUFFER, 1);
    if (ret < 0)
        return ret;
    msg[0] = params;
    msg[1] = (28u << 16) | op->opcode;
    msg[2] = buf;
    msg[3] = (uint32_t)e->width;
    msg[4] = (uint32_t)e->height;
    msg[5] = e->drm_format;
    msg[6] = 0;
    ret = send_u32s(&s->conn, msg, 7);
    if (ret < 0)
        return ret;

    if (req_op(&s->proto, "zwp_linux_buffer_params_v1", "destroy", &op) == 0) {
        msg[0] = params;
        msg[1] = (8u << 16) | op->opcode;
        ret = send_u32s(&s->conn, msg, 2);
        if (ret < 0)
            return ret;
        wp_map_del(&s->conn.map, params);
    }
    *out_id = buf;
    return 0;
}

int wp_session_destroy_wl_buffer(struct wp_session *s, uint32_t id)
{
    const struct wp_proto_msg *op;
    uint32_t msg[2];

    if (!s || id == 0)
        return -EINVAL;
    if (req_op(&s->proto, "wl_buffer", "destroy", &op) < 0)
        return -ENOENT;
    msg[0] = id;
    msg[1] = (8u << 16) | op->opcode;
    wp_map_del(&s->conn.map, id);
    return send_u32s(&s->conn, msg, 2);
}

int wp_session_commit_buffer(struct wp_session *s, uint32_t buffer,
                             uint32_t acq_timeline, uint64_t acq_pt,
                             uint32_t rel_timeline, uint64_t rel_pt)
{
    const struct wp_proto_msg *op;
    uint32_t msg[8];
    int ret;

    if (!s || buffer == 0 || acq_timeline == 0 || rel_timeline == 0)
        return -EINVAL;

    if (req_op(&s->proto, "wp_linux_drm_syncobj_surface_v1", "set_acquire_point", &op) < 0)
        return -ENOENT;
    msg[0] = s->syncobj_surface;
    msg[1] = (20u << 16) | op->opcode;
    msg[2] = acq_timeline;
    msg[3] = (uint32_t)(acq_pt >> 32);
    msg[4] = (uint32_t)(acq_pt & 0xffffffffu);
    ret = send_u32s(&s->conn, msg, 5);
    if (ret < 0)
        return ret;

    if (req_op(&s->proto, "wp_linux_drm_syncobj_surface_v1", "set_release_point", &op) < 0)
        return -ENOENT;
    msg[0] = s->syncobj_surface;
    msg[1] = (20u << 16) | op->opcode;
    msg[2] = rel_timeline;
    msg[3] = (uint32_t)(rel_pt >> 32);
    msg[4] = (uint32_t)(rel_pt & 0xffffffffu);
    ret = send_u32s(&s->conn, msg, 5);
    if (ret < 0)
        return ret;

    if (req_op(&s->proto, "wl_surface", "set_buffer_scale", &op) == 0) {
        int32_t sc = s->scale > 0 ? s->scale : 1;
        msg[0] = s->surface;
        msg[1] = (12u << 16) | op->opcode;
        msg[2] = (uint32_t)sc;
        ret = send_u32s(&s->conn, msg, 3);
        if (ret < 0)
            return ret;
    }

    if (req_op(&s->proto, "wl_surface", "attach", &op) < 0)
        return -ENOENT;
    msg[0] = s->surface;
    msg[1] = (20u << 16) | op->opcode;
    msg[2] = buffer;
    msg[3] = 0;
    msg[4] = 0;
    ret = send_u32s(&s->conn, msg, 5);
    if (ret < 0)
        return ret;

    if (req_op(&s->proto, "wl_surface", "damage_buffer", &op) < 0)
        return -ENOENT;
    msg[0] = s->surface;
    msg[1] = (24u << 16) | op->opcode;
    msg[2] = 0;
    msg[3] = 0;
    msg[4] = s->buf_w ? s->buf_w : (uint32_t)s->width;
    msg[5] = s->buf_h ? s->buf_h : (uint32_t)s->height;
    ret = send_u32s(&s->conn, msg, 6);
    if (ret < 0)
        return ret;

    ret = wp_session_request_frame(s);
    if (ret < 0)
        return ret;

    if (req_op(&s->proto, "wl_surface", "commit", &op) < 0)
        return -ENOENT;
    msg[0] = s->surface;
    msg[1] = (8u << 16) | op->opcode;
    return send_u32s(&s->conn, msg, 2);
}

int wp_session_request_frame(struct wp_session *s)
{
    const struct wp_proto_msg *op;
    uint32_t msg[3];
    int ret;

    if (!s)
        return -EINVAL;
    if (s->frame_cb != 0)
        return 0;
    if (req_op(&s->proto, "wl_surface", "frame", &op) < 0)
        return -ENOENT;
    s->frame_cb = wp_wl_alloc_id(&s->conn);
    ret = wp_map_set(&s->conn.map, s->frame_cb, WP_OBJ_CALLBACK, 1);
    if (ret < 0)
        return ret;
    s->frame_done = false;
    msg[0] = s->surface;
    msg[1] = (12u << 16) | op->opcode;
    msg[2] = s->frame_cb;
    return send_u32s(&s->conn, msg, 3);
}

int wp_session_ack(struct wp_session *s)
{
    const struct wp_proto_msg *op;
    uint32_t msg[3];

    if (!s || !s->configure_serial)
        return -EINVAL;
    if (req_op(&s->proto, "xdg_surface", "ack_configure", &op) < 0)
        return -ENOENT;
    msg[0] = s->xdg_surface;
    msg[1] = (12u << 16) | op->opcode;
    msg[2] = s->configure_serial;
    return send_u32s(&s->conn, msg, 3);
}

static int toplevel_void(struct wp_session *s, const char *req)
{
    const struct wp_proto_msg *op;
    uint32_t msg[2];

    if (!s || !s->xdg_toplevel)
        return -EINVAL;
    if (req_op(&s->proto, "xdg_toplevel", req, &op) < 0)
        return -ENOENT;
    msg[0] = s->xdg_toplevel;
    msg[1] = (8u << 16) | op->opcode;
    return send_u32s(&s->conn, msg, 2);
}

int wp_session_toplevel_move(struct wp_session *s)
{
    const struct wp_proto_msg *op;
    uint32_t msg[4];

    if (!s || !s->xdg_toplevel || !s->seat || s->pointer_serial == 0)
        return -EINVAL;
    if (req_op(&s->proto, "xdg_toplevel", "move", &op) < 0)
        return -ENOENT;
    msg[0] = s->xdg_toplevel;
    msg[1] = (16u << 16) | op->opcode;
    msg[2] = s->seat;
    msg[3] = s->pointer_serial;
    return send_u32s(&s->conn, msg, 4);
}

int wp_session_toplevel_resize(struct wp_session *s, uint32_t edges)
{
    const struct wp_proto_msg *op;
    uint32_t msg[5];

    if (!s || !s->xdg_toplevel || !s->seat || s->pointer_serial == 0)
        return -EINVAL;
    if (req_op(&s->proto, "xdg_toplevel", "resize", &op) < 0)
        return -ENOENT;
    msg[0] = s->xdg_toplevel;
    msg[1] = (20u << 16) | op->opcode;
    msg[2] = s->seat;
    msg[3] = s->pointer_serial;
    msg[4] = edges;
    return send_u32s(&s->conn, msg, 5);
}

int wp_session_toplevel_set_maximized(struct wp_session *s, bool on)
{
    return toplevel_void(s, on ? "set_maximized" : "unset_maximized");
}

int wp_session_toplevel_set_fullscreen(struct wp_session *s, bool on)
{
    return toplevel_void(s, on ? "set_fullscreen" : "unset_fullscreen");
}

void wp_session_close(struct wp_session *s)
{
    if (!s)
        return;
    wp_feedback_free(&s->fb);
    wp_registry_free(&s->reg);
    wp_wl_conn_destroy(&s->conn);
    wp_proto_free(&s->proto);
    memset(s, 0, sizeof(*s));
}

void wp_session_print(const struct wp_session *s, FILE *out)
{
    if (!s || !out)
        return;
    fprintf(out,
            "surface %u  xdg %dx%d scale %d buf %ux%u serial %u  states 0x%x  configured %s\n",
            s->surface, s->width, s->height, s->scale, s->buf_w, s->buf_h,
            s->configure_serial, s->toplevel_states, s->configured ? "yes" : "no");
    wp_feedback_print(&s->fb, out);
}

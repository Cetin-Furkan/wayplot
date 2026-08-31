#define _GNU_SOURCE
#include "wayland/proto.h"

#include <stdlib.h>
#include <string.h>

struct builder {
    uint8_t *base;
    size_t cap;
    size_t used;
};

static int grow(struct builder *b, size_t need)
{
    size_t cap = b->cap ? b->cap : 256;
    uint8_t *p;
    while (cap < need)
        cap *= 2;
    p = realloc(b->base, cap);
    if (!p)
        return -1;
    b->base = p;
    b->cap = cap;
    return 0;
}

static uint32_t bump(struct builder *b, size_t n, size_t align)
{
    size_t used = (b->used + align - 1) & ~(align - 1);
    if (grow(b, used + n) < 0)
        return UINT32_MAX;
    if (n)
        memset(b->base + used, 0, n);
    b->used = used + n;
    return (uint32_t)used;
}

static uint32_t intern(struct builder *b, const char *s)
{
    size_t n = strlen(s) + 1;
    uint32_t off = bump(b, n, 1);
    if (off == UINT32_MAX)
        return UINT32_MAX;
    memcpy(b->base + off, s, n);
    return off;
}

static const struct wp_proto_iface *iface_at(const struct wp_proto *p, uint32_t i)
{
    return (const struct wp_proto_iface *)(p->base + p->ifaces_off) + i;
}

static const struct wp_proto_msg *msg_at(const struct wp_proto *p, uint32_t off, uint16_t i)
{
    return (const struct wp_proto_msg *)(p->base + off) + i;
}

static void set_msg(struct wp_proto_msg *m, uint32_t name, uint16_t op, uint16_t nargs)
{
    *m = (struct wp_proto_msg){ .name_off = name, .opcode = op, .nargs = nargs };
}

int wp_proto_core(struct wp_proto *p)
{
    struct builder b = { 0 };
    uint32_t s_sync, s_getreg, s_error, s_del, s_bind, s_global, s_grem, s_done;
    uint32_t s_display, s_registry, s_callback, s_comp, s_surf, s_xdgwm, s_xdgs, s_xdgt;
    uint32_t s_dmabuf, s_params, s_buf, s_fb, s_syncm, s_synctl, s_syncsf;
    uint32_t s_csurf, s_commit, s_attach, s_dmgb, s_frame;
    uint32_t s_getxs, s_pong, s_ping, s_gett, s_ack, s_cfg, s_title, s_close;
    uint32_t s_gsf, s_cpar, s_add, s_cimmed, s_destroy;
    uint32_t s_ft, s_mdev, s_tdone, s_ttdev, s_tfmt, s_tflags;
    uint32_t s_gsurf, s_import, s_acq, s_rel;
    uint32_t display_reqs, display_ev, reg_reqs, reg_ev, cb_ev;
    uint32_t comp_reqs, surf_reqs, surf_ev, xdgwm_reqs, xdgwm_ev, xdgs_reqs, xdgs_ev;
    uint32_t xdgt_reqs, xdgt_ev, dmabuf_reqs, params_reqs, buf_reqs;
    uint32_t fb_ev, syncm_reqs, synctl_reqs, syncsf_reqs, ifaces;
    uint32_t output_ev, seat_reqs, seat_ev, ptr_ev;
    uint32_t csmgr_reqs, csdev_reqs;
    struct wp_proto_msg *m;
    struct wp_proto_iface *ifc;

    if (!p)
        return -1;
    memset(p, 0, sizeof(*p));
    if (bump(&b, 4, 4) == UINT32_MAX)
        goto fail;

    s_sync = intern(&b, "sync");
    s_getreg = intern(&b, "get_registry");
    s_error = intern(&b, "error");
    s_del = intern(&b, "delete_id");
    s_bind = intern(&b, "bind");
    s_global = intern(&b, "global");
    s_grem = intern(&b, "global_remove");
    s_done = intern(&b, "done");
    s_display = intern(&b, "wl_display");
    s_registry = intern(&b, "wl_registry");
    s_callback = intern(&b, "wl_callback");
    s_comp = intern(&b, "wl_compositor");
    s_surf = intern(&b, "wl_surface");
    s_xdgwm = intern(&b, "xdg_wm_base");
    s_xdgs = intern(&b, "xdg_surface");
    s_xdgt = intern(&b, "xdg_toplevel");
    s_dmabuf = intern(&b, "zwp_linux_dmabuf_v1");
    s_params = intern(&b, "zwp_linux_buffer_params_v1");
    s_buf = intern(&b, "wl_buffer");
    s_fb = intern(&b, "zwp_linux_dmabuf_feedback_v1");
    s_syncm = intern(&b, "wp_linux_drm_syncobj_manager_v1");
    s_synctl = intern(&b, "wp_linux_drm_syncobj_timeline_v1");
    s_syncsf = intern(&b, "wp_linux_drm_syncobj_surface_v1");
    s_csurf = intern(&b, "create_surface");
    s_commit = intern(&b, "commit");
    s_attach = intern(&b, "attach");
    s_dmgb = intern(&b, "damage_buffer");
    s_frame = intern(&b, "frame");
    s_getxs = intern(&b, "get_xdg_surface");
    s_pong = intern(&b, "pong");
    s_ping = intern(&b, "ping");
    s_gett = intern(&b, "get_toplevel");
    s_ack = intern(&b, "ack_configure");
    s_cfg = intern(&b, "configure");
    s_title = intern(&b, "set_title");
    s_close = intern(&b, "close");
    s_gsf = intern(&b, "get_surface_feedback");
    s_cpar = intern(&b, "create_params");
    s_add = intern(&b, "add");
    s_cimmed = intern(&b, "create_immed");
    s_destroy = intern(&b, "destroy");
    uint32_t s_create = intern(&b, "create");
    s_ft = intern(&b, "format_table");
    s_mdev = intern(&b, "main_device");
    s_tdone = intern(&b, "tranche_done");
    s_ttdev = intern(&b, "tranche_target_device");
    s_tfmt = intern(&b, "tranche_formats");
    s_tflags = intern(&b, "tranche_flags");
    s_gsurf = intern(&b, "get_surface");
    s_import = intern(&b, "import_timeline");
    s_acq = intern(&b, "set_acquire_point");
    s_rel = intern(&b, "set_release_point");
    uint32_t s_setscale = intern(&b, "set_buffer_scale");
    uint32_t s_enter = intern(&b, "enter");
    uint32_t s_leave = intern(&b, "leave");
    uint32_t s_prefscale = intern(&b, "preferred_buffer_scale");
    uint32_t s_move = intern(&b, "move");
    uint32_t s_resize = intern(&b, "resize");
    uint32_t s_maxsz = intern(&b, "set_max_size");
    uint32_t s_minsz = intern(&b, "set_min_size");
    uint32_t s_max = intern(&b, "set_maximized");
    uint32_t s_unmax = intern(&b, "unset_maximized");
    uint32_t s_full = intern(&b, "set_fullscreen");
    uint32_t s_unfull = intern(&b, "unset_fullscreen");
    uint32_t s_geom = intern(&b, "geometry");
    uint32_t s_mode = intern(&b, "mode");
    uint32_t s_scale = intern(&b, "scale");
    uint32_t s_caps = intern(&b, "capabilities");
    uint32_t s_getptr = intern(&b, "get_pointer");
    uint32_t s_motion = intern(&b, "motion");
    uint32_t s_button = intern(&b, "button");
    uint32_t s_axis = intern(&b, "axis");
    uint32_t s_output = intern(&b, "wl_output");
    uint32_t s_seat = intern(&b, "wl_seat");
    uint32_t s_pointer = intern(&b, "wl_pointer");
    uint32_t s_csmgr = intern(&b, "wp_cursor_shape_manager_v1");
    uint32_t s_csdev = intern(&b, "wp_cursor_shape_device_v1");
    uint32_t s_setshape = intern(&b, "set_shape");
    if (s_setshape == UINT32_MAX || s_axis == UINT32_MAX)
        goto fail;

    display_reqs = bump(&b, sizeof(struct wp_proto_msg) * 2, 4);
    display_ev = bump(&b, sizeof(struct wp_proto_msg) * 2, 4);
    reg_reqs = bump(&b, sizeof(struct wp_proto_msg) * 1, 4);
    reg_ev = bump(&b, sizeof(struct wp_proto_msg) * 2, 4);
    cb_ev = bump(&b, sizeof(struct wp_proto_msg) * 1, 4);
    comp_reqs = bump(&b, sizeof(struct wp_proto_msg) * 1, 4);
    surf_reqs = bump(&b, sizeof(struct wp_proto_msg) * 5, 4);
    surf_ev = bump(&b, sizeof(struct wp_proto_msg) * 3, 4);
    xdgwm_reqs = bump(&b, sizeof(struct wp_proto_msg) * 2, 4);
    xdgwm_ev = bump(&b, sizeof(struct wp_proto_msg) * 1, 4);
    xdgs_reqs = bump(&b, sizeof(struct wp_proto_msg) * 2, 4);
    xdgs_ev = bump(&b, sizeof(struct wp_proto_msg) * 1, 4);
    xdgt_reqs = bump(&b, sizeof(struct wp_proto_msg) * 9, 4);
    xdgt_ev = bump(&b, sizeof(struct wp_proto_msg) * 2, 4);
    dmabuf_reqs = bump(&b, sizeof(struct wp_proto_msg) * 2, 4);
    params_reqs = bump(&b, sizeof(struct wp_proto_msg) * 4, 4);
    buf_reqs = bump(&b, sizeof(struct wp_proto_msg) * 1, 4);
    fb_ev = bump(&b, sizeof(struct wp_proto_msg) * 7, 4);
    syncm_reqs = bump(&b, sizeof(struct wp_proto_msg) * 2, 4);
    synctl_reqs = bump(&b, sizeof(struct wp_proto_msg) * 1, 4);
    syncsf_reqs = bump(&b, sizeof(struct wp_proto_msg) * 2, 4);
    output_ev = bump(&b, sizeof(struct wp_proto_msg) * 4, 4);
    seat_reqs = bump(&b, sizeof(struct wp_proto_msg) * 1, 4);
    seat_ev = bump(&b, sizeof(struct wp_proto_msg) * 1, 4);
    ptr_ev = bump(&b, sizeof(struct wp_proto_msg) * 5, 4);
    csmgr_reqs = bump(&b, sizeof(struct wp_proto_msg) * 1, 4);
    csdev_reqs = bump(&b, sizeof(struct wp_proto_msg) * 1, 4);
    ifaces = bump(&b, sizeof(struct wp_proto_iface) * 20, 4);
    if (ifaces == UINT32_MAX)
        goto fail;

    m = (struct wp_proto_msg *)(b.base + display_reqs);
    set_msg(&m[0], s_sync, 0, 1);
    set_msg(&m[1], s_getreg, 1, 1);
    m = (struct wp_proto_msg *)(b.base + display_ev);
    set_msg(&m[0], s_error, 0, 3);
    set_msg(&m[1], s_del, 1, 1);
    m = (struct wp_proto_msg *)(b.base + reg_reqs);
    set_msg(&m[0], s_bind, 0, 3);
    m = (struct wp_proto_msg *)(b.base + reg_ev);
    set_msg(&m[0], s_global, 0, 3);
    set_msg(&m[1], s_grem, 1, 1);
    m = (struct wp_proto_msg *)(b.base + cb_ev);
    set_msg(&m[0], s_done, 0, 1);
    m = (struct wp_proto_msg *)(b.base + comp_reqs);
    set_msg(&m[0], s_csurf, 0, 1);
    m = (struct wp_proto_msg *)(b.base + surf_reqs);
    set_msg(&m[0], s_attach, 1, 3);
    set_msg(&m[1], s_frame, 3, 1);
    set_msg(&m[2], s_commit, 6, 0);
    set_msg(&m[3], s_dmgb, 9, 4);
    set_msg(&m[4], s_setscale, 8, 1);
    m = (struct wp_proto_msg *)(b.base + surf_ev);
    set_msg(&m[0], s_enter, 0, 1);
    set_msg(&m[1], s_leave, 1, 1);
    set_msg(&m[2], s_prefscale, 2, 1);
    m = (struct wp_proto_msg *)(b.base + xdgwm_reqs);
    set_msg(&m[0], s_getxs, 2, 2);
    set_msg(&m[1], s_pong, 3, 1);
    m = (struct wp_proto_msg *)(b.base + xdgwm_ev);
    set_msg(&m[0], s_ping, 0, 1);
    m = (struct wp_proto_msg *)(b.base + xdgs_reqs);
    set_msg(&m[0], s_gett, 1, 1);
    set_msg(&m[1], s_ack, 4, 1);
    m = (struct wp_proto_msg *)(b.base + xdgs_ev);
    set_msg(&m[0], s_cfg, 0, 1);
    m = (struct wp_proto_msg *)(b.base + xdgt_reqs);
    set_msg(&m[0], s_title, 2, 1);
    set_msg(&m[1], s_move, 5, 2);
    set_msg(&m[2], s_resize, 6, 3);
    set_msg(&m[3], s_maxsz, 7, 2);
    set_msg(&m[4], s_minsz, 8, 2);
    set_msg(&m[5], s_max, 9, 0);
    set_msg(&m[6], s_unmax, 10, 0);
    set_msg(&m[7], s_full, 11, 1);
    set_msg(&m[8], s_unfull, 12, 0);
    m = (struct wp_proto_msg *)(b.base + xdgt_ev);
    set_msg(&m[0], s_cfg, 0, 3);
    set_msg(&m[1], s_close, 1, 0);
    m = (struct wp_proto_msg *)(b.base + dmabuf_reqs);
    set_msg(&m[0], s_cpar, 1, 1);
    set_msg(&m[1], s_gsf, 3, 2);
    m = (struct wp_proto_msg *)(b.base + params_reqs);
    set_msg(&m[0], s_destroy, 0, 0);
    set_msg(&m[1], s_add, 1, 5);
    set_msg(&m[2], s_create, 2, 5);
    set_msg(&m[3], s_cimmed, 3, 6);
    m = (struct wp_proto_msg *)(b.base + buf_reqs);
    set_msg(&m[0], s_destroy, 0, 0);
    m = (struct wp_proto_msg *)(b.base + fb_ev);
    set_msg(&m[0], s_done, 0, 0);
    set_msg(&m[1], s_ft, 1, 1);
    set_msg(&m[2], s_mdev, 2, 1);
    set_msg(&m[3], s_tdone, 3, 0);
    set_msg(&m[4], s_ttdev, 4, 1);
    set_msg(&m[5], s_tfmt, 5, 1);
    set_msg(&m[6], s_tflags, 6, 1);
    m = (struct wp_proto_msg *)(b.base + syncm_reqs);
    set_msg(&m[0], s_gsurf, 1, 2);
    set_msg(&m[1], s_import, 2, 1);
    m = (struct wp_proto_msg *)(b.base + synctl_reqs);
    set_msg(&m[0], s_destroy, 0, 0);
    m = (struct wp_proto_msg *)(b.base + syncsf_reqs);
    set_msg(&m[0], s_acq, 1, 3);
    set_msg(&m[1], s_rel, 2, 3);
    m = (struct wp_proto_msg *)(b.base + output_ev);
    set_msg(&m[0], s_geom, 0, 8);
    set_msg(&m[1], s_mode, 1, 4);
    set_msg(&m[2], s_done, 2, 0);
    set_msg(&m[3], s_scale, 3, 1);
    m = (struct wp_proto_msg *)(b.base + seat_reqs);
    set_msg(&m[0], s_getptr, 0, 1);
    m = (struct wp_proto_msg *)(b.base + seat_ev);
    set_msg(&m[0], s_caps, 0, 1);
    m = (struct wp_proto_msg *)(b.base + ptr_ev);
    set_msg(&m[0], s_enter, 0, 4);
    set_msg(&m[1], s_leave, 1, 2);
    set_msg(&m[2], s_motion, 2, 3);
    set_msg(&m[3], s_button, 3, 4);
    set_msg(&m[4], s_axis, 4, 3);
    m = (struct wp_proto_msg *)(b.base + csmgr_reqs);
    set_msg(&m[0], s_getptr, 1, 2);
    m = (struct wp_proto_msg *)(b.base + csdev_reqs);
    set_msg(&m[0], s_setshape, 1, 2);

    ifc = (struct wp_proto_iface *)(b.base + ifaces);
    ifc[0] = (struct wp_proto_iface){ .name_off = s_display, .version = 1, .nrequests = 2, .nevents = 2, .requests_off = display_reqs, .events_off = display_ev };
    ifc[1] = (struct wp_proto_iface){ .name_off = s_registry, .version = 1, .nrequests = 1, .nevents = 2, .requests_off = reg_reqs, .events_off = reg_ev };
    ifc[2] = (struct wp_proto_iface){ .name_off = s_callback, .version = 1, .nrequests = 0, .nevents = 1, .events_off = cb_ev };
    ifc[3] = (struct wp_proto_iface){ .name_off = s_comp, .version = 6, .nrequests = 1, .requests_off = comp_reqs };
    ifc[4] = (struct wp_proto_iface){ .name_off = s_surf, .version = 6, .nrequests = 5, .nevents = 3, .requests_off = surf_reqs, .events_off = surf_ev };
    ifc[5] = (struct wp_proto_iface){ .name_off = s_xdgwm, .version = 6, .nrequests = 2, .nevents = 1, .requests_off = xdgwm_reqs, .events_off = xdgwm_ev };
    ifc[6] = (struct wp_proto_iface){ .name_off = s_xdgs, .version = 6, .nrequests = 2, .nevents = 1, .requests_off = xdgs_reqs, .events_off = xdgs_ev };
    ifc[7] = (struct wp_proto_iface){ .name_off = s_xdgt, .version = 6, .nrequests = 9, .nevents = 2, .requests_off = xdgt_reqs, .events_off = xdgt_ev };
    ifc[8] = (struct wp_proto_iface){ .name_off = s_dmabuf, .version = 4, .nrequests = 2, .requests_off = dmabuf_reqs };
    ifc[9] = (struct wp_proto_iface){ .name_off = s_fb, .version = 4, .nevents = 7, .events_off = fb_ev };
    ifc[10] = (struct wp_proto_iface){ .name_off = s_syncm, .version = 1, .nrequests = 2, .requests_off = syncm_reqs };
    ifc[11] = (struct wp_proto_iface){ .name_off = s_params, .version = 3, .nrequests = 4, .requests_off = params_reqs };
    ifc[12] = (struct wp_proto_iface){ .name_off = s_buf, .version = 1, .nrequests = 1, .requests_off = buf_reqs };
    ifc[13] = (struct wp_proto_iface){ .name_off = s_synctl, .version = 1, .nrequests = 1, .requests_off = synctl_reqs };
    ifc[14] = (struct wp_proto_iface){ .name_off = s_syncsf, .version = 1, .nrequests = 2, .requests_off = syncsf_reqs };
    ifc[15] = (struct wp_proto_iface){ .name_off = s_output, .version = 4, .nrequests = 0, .nevents = 4, .events_off = output_ev };
    ifc[16] = (struct wp_proto_iface){ .name_off = s_seat, .version = 8, .nrequests = 1, .nevents = 1, .requests_off = seat_reqs, .events_off = seat_ev };
    ifc[17] = (struct wp_proto_iface){ .name_off = s_pointer, .version = 8, .nrequests = 0, .nevents = 5, .events_off = ptr_ev };
    ifc[18] = (struct wp_proto_iface){ .name_off = s_csmgr, .version = 2, .nrequests = 1, .requests_off = csmgr_reqs };
    ifc[19] = (struct wp_proto_iface){ .name_off = s_csdev, .version = 1, .nrequests = 1, .requests_off = csdev_reqs };

    p->base = b.base;
    p->size = b.used;
    p->niface = 20;
    p->ifaces_off = ifaces;
    return 0;
fail:
    free(b.base);
    return -1;
}

void wp_proto_free(struct wp_proto *p)
{
    if (!p)
        return;
    free(p->base);
    memset(p, 0, sizeof(*p));
}

const char *wp_proto_str(const struct wp_proto *p, uint32_t off)
{
    if (!p || !p->base || off >= p->size)
        return "";
    return (const char *)(p->base + off);
}

const struct wp_proto_iface *wp_proto_iface(const struct wp_proto *p, const char *name)
{
    uint32_t i;
    if (!p || !name)
        return NULL;
    for (i = 0; i < p->niface; i++) {
        const struct wp_proto_iface *ifc = iface_at(p, i);
        if (strcmp(wp_proto_str(p, ifc->name_off), name) == 0)
            return ifc;
    }
    return NULL;
}

static const struct wp_proto_msg *find_msg(const struct wp_proto *p, uint32_t off,
                                           uint16_t n, const char *name)
{
    uint16_t i;
    if (!off || !n)
        return NULL;
    for (i = 0; i < n; i++) {
        const struct wp_proto_msg *m = msg_at(p, off, i);
        if (strcmp(wp_proto_str(p, m->name_off), name) == 0)
            return m;
    }
    return NULL;
}

const struct wp_proto_msg *wp_proto_request(const struct wp_proto *p, const char *iface, const char *req)
{
    const struct wp_proto_iface *ifc = wp_proto_iface(p, iface);
    if (!ifc)
        return NULL;
    return find_msg(p, ifc->requests_off, ifc->nrequests, req);
}

const struct wp_proto_msg *wp_proto_event(const struct wp_proto *p, const char *iface, const char *ev)
{
    const struct wp_proto_iface *ifc = wp_proto_iface(p, iface);
    if (!ifc)
        return NULL;
    return find_msg(p, ifc->events_off, ifc->nevents, ev);
}

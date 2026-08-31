#include "wayland/proto.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

static void expect(int cond, const char *what)
{
    if (cond)
        printf("PASS  %s\n", what);
    else {
        printf("FAIL  %s\n", what);
        g_fail++;
    }
}

int main(void)
{
    struct wp_proto p;
    const struct wp_proto_msg *m;
    const struct wp_proto_iface *ifc;

    expect(wp_proto_core(&p) == 0, "wp_proto_core (relative-pointer blob, no XML)");
    ifc = wp_proto_iface(&p, "wl_display");
    expect(ifc && ifc->nrequests == 2 && ifc->nevents == 2, "wl_display in blob");
    m = wp_proto_request(&p, "wl_display", "sync");
    expect(m && m->opcode == 0, "display.sync opcode 0");
    m = wp_proto_request(&p, "wl_display", "get_registry");
    expect(m && m->opcode == 1, "display.get_registry opcode 1");
    m = wp_proto_event(&p, "wl_registry", "global");
    expect(m && m->opcode == 0, "registry.global opcode 0");
    m = wp_proto_request(&p, "wl_registry", "bind");
    expect(m && m->opcode == 0, "registry.bind opcode 0");
    m = wp_proto_event(&p, "wl_callback", "done");
    expect(m && m->opcode == 0, "callback.done opcode 0");
    m = wp_proto_request(&p, "wl_compositor", "create_surface");
    expect(m && m->opcode == 0, "compositor.create_surface opcode 0");
    m = wp_proto_request(&p, "zwp_linux_dmabuf_v1", "get_surface_feedback");
    expect(m && m->opcode == 3, "dmabuf.get_surface_feedback opcode 3");
    m = wp_proto_request(&p, "zwp_linux_dmabuf_v1", "create_params");
    expect(m && m->opcode == 1, "dmabuf.create_params opcode 1");
    m = wp_proto_request(&p, "zwp_linux_buffer_params_v1", "add");
    expect(m && m->opcode == 1, "params.add opcode 1");
    m = wp_proto_request(&p, "zwp_linux_buffer_params_v1", "create_immed");
    expect(m && m->opcode == 3, "params.create_immed opcode 3");
    m = wp_proto_request(&p, "wl_surface", "attach");
    expect(m && m->opcode == 1, "surface.attach opcode 1");
    m = wp_proto_request(&p, "wl_surface", "damage_buffer");
    expect(m && m->opcode == 9, "surface.damage_buffer opcode 9");
    m = wp_proto_request(&p, "wl_surface", "frame");
    expect(m && m->opcode == 3, "surface.frame opcode 3");
    m = wp_proto_request(&p, "wl_surface", "set_buffer_scale");
    expect(m && m->opcode == 8, "surface.set_buffer_scale opcode 8");
    m = wp_proto_event(&p, "wl_surface", "preferred_buffer_scale");
    expect(m && m->opcode == 2, "surface.preferred_buffer_scale opcode 2");
    m = wp_proto_event(&p, "xdg_toplevel", "configure");
    expect(m && m->opcode == 0, "xdg_toplevel.configure opcode 0");
    m = wp_proto_event(&p, "xdg_toplevel", "close");
    expect(m && m->opcode == 1, "xdg_toplevel.close opcode 1");
    m = wp_proto_request(&p, "xdg_toplevel", "set_min_size");
    expect(m && m->opcode == 8, "xdg_toplevel.set_min_size opcode 8");
    m = wp_proto_request(&p, "xdg_toplevel", "resize");
    expect(m && m->opcode == 6, "xdg_toplevel.resize opcode 6");
    m = wp_proto_event(&p, "wl_output", "scale");
    expect(m && m->opcode == 3, "wl_output.scale opcode 3");
    m = wp_proto_event(&p, "wl_pointer", "axis");
    expect(m && m->opcode == 4, "wl_pointer.axis opcode 4");
    m = wp_proto_request(&p, "wp_cursor_shape_manager_v1", "get_pointer");
    expect(m && m->opcode == 1, "cursor_shape_manager.get_pointer opcode 1");
    m = wp_proto_request(&p, "wp_cursor_shape_device_v1", "set_shape");
    expect(m && m->opcode == 1, "cursor_shape_device.set_shape opcode 1");
    m = wp_proto_request(&p, "wp_linux_drm_syncobj_manager_v1", "get_surface");
    expect(m && m->opcode == 1, "syncobj.get_surface opcode 1");
    m = wp_proto_request(&p, "wp_linux_drm_syncobj_manager_v1", "import_timeline");
    expect(m && m->opcode == 2, "syncobj.import_timeline opcode 2");
    m = wp_proto_request(&p, "wp_linux_drm_syncobj_surface_v1", "set_acquire_point");
    expect(m && m->opcode == 1, "syncobj_surface.set_acquire_point opcode 1");
    m = wp_proto_request(&p, "wp_linux_drm_syncobj_surface_v1", "set_release_point");
    expect(m && m->opcode == 2, "syncobj_surface.set_release_point opcode 2");
    expect(wp_proto_request(&p, "wl_display", "nope") == NULL, "unknown request is NULL");
    wp_proto_free(&p);
    printf("%s  %d failure(s)\n", g_fail ? "RESULT FAIL" : "RESULT PASS", g_fail);
    return g_fail ? 1 : 0;
}

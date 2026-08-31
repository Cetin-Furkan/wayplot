#define _GNU_SOURCE
#include "wayland/session.h"

#include <errno.h>
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
    struct wp_session s;
    int ret;

    ret = wp_session_open(&s);
    expect(ret == 0, "session_open (connect + registry)");
    if (ret < 0) {
        printf("      open %s\n", strerror(-ret));
        return 1;
    }
    expect(s.reg.count >= 4, "globals from map-backed registry");

    ret = wp_session_setup_surface(&s);
    expect(ret == 0, "bind compositor/xdg/dmabuf + surface + feedback + configure");
    if (ret < 0)
        printf("      setup_surface %s\n", strerror(-ret));
    else {
        wp_session_print(&s, stdout);
        expect(s.configured && s.configure_serial, "xdg_surface.configure acked");
        expect(s.fb.done, "zwp_linux_dmabuf_feedback_v1.done");
        expect(s.fb.table && s.fb.table_count > 0, "owned copy of format_table");
        expect(s.fb.ntranches > 0, "at least one tranche");
        expect(s.fb.main_device != 0, "main_device nonzero");
        expect(s.fb.tranches && s.fb.tranches[0].pairs && s.fb.tranches[0].pair_count > 0,
               "tranche indices expanded to format+modifier pairs");
        expect(s.width >= 1 && s.height >= 1, "logical size is at least 1x1 (0x0 becomes default)");
        expect(s.scale >= 1, "integer scale >= 1");
        {
            uint32_t bw, bh;
            wp_session_buffer_size(&s, &bw, &bh);
            expect(bw == (uint32_t)s.width * (uint32_t)s.scale &&
                       bh == (uint32_t)s.height * (uint32_t)s.scale,
                   "buffer = logical × scale");
            expect(s.buf_w == bw && s.buf_h == bh, "session buf_w/h match");
        }
        {
            uint32_t st[3] = { 1, 5, 4 };
            uint32_t m = wp_xdg_states_mask(st, sizeof(st));
            expect((m & WP_TOPLEVEL_MAXIMIZED) && (m & WP_TOPLEVEL_TILED) &&
                       (m & WP_TOPLEVEL_ACTIVATED),
                   "xdg states array → maximized+tiled+activated");
        }
        expect(wp_pointer_edge_mask(0, 0, 1280, 720) == WP_RESIZE_TOP_LEFT,
               "corner (0,0) is top-left resize");
        expect(wp_pointer_edge_mask(640, 360, 1280, 720) == WP_RESIZE_NONE,
               "center is not a resize edge");
        expect(wp_pointer_edge_mask(1279, 719, 1280, 720) == WP_RESIZE_BOTTOM_RIGHT,
               "far corner is bottom-right resize");
        expect(wp_pointer_cursor_shape(WP_RESIZE_RIGHT) == WP_CURSOR_E_RESIZE,
               "right edge uses e-resize cursor, not default");
        expect(s.seat != 0, "wl_seat bound (pointer events exist)");
        expect(s.pointer_buttons == 0 && s.pointer_pressed == 0 && s.pointer_released == 0,
               "pointer snapshot starts idle (host reads edges after pump)");
        if (wp_registry_find(&s.reg, "wp_cursor_shape_manager_v1"))
            expect(s.cursor_shape_mgr != 0, "cursor-shape manager bound so GNOME can show a pointer");
    }

    wp_session_close(&s);
    printf("%s  %d failure(s)\n", g_fail ? "RESULT FAIL" : "RESULT PASS", g_fail);
    return g_fail ? 1 : 0;
}

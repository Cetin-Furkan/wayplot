#define _GNU_SOURCE
#include "wayland/conn.h"
#include "wayland/proto.h"
#include "wayland/registry.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
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
    char path[256];
    struct wp_wl_conn *conn;
    struct wp_proto proto;
    struct wp_registry reg;
    const struct wp_global *g;
    int ret;

    conn = calloc(1, sizeof(*conn));
    if (!conn) {
        printf("FAIL  alloc conn\n");
        return 1;
    }
    conn->fd = -1;

    ret = wp_wl_display_path(path, sizeof(path));
    expect(ret == 0, "XDG_RUNTIME_DIR + WAYLAND_DISPLAY");
    if (ret < 0) {
        printf("      no wayland display in environment (%s)\n", strerror(-ret));
        free(conn);
        return 1;
    }
    printf("      path %s\n", path);

    ret = wp_proto_core(&proto);
    expect(ret == 0, "protocol blob");

    ret = wp_wl_conn_connect(conn, path);
    expect(ret == 0, "IORING_OP_SOCKET + CONNECT (or libc fallback) to compositor");
    if (ret < 0) {
        printf("      connect %s\n", strerror(-ret));
        wp_proto_free(&proto);
        wp_wl_conn_destroy(conn);
        free(conn);
        return 1;
    }
    printf("      ring no_sqarray=%s enter_flags=0x%x\n",
           conn->ring.no_sqarray ? "yes" : "no", conn->ring.enter_flags);

    memset(&reg, 0, sizeof(reg));
    ret = wp_registry_roundtrip(&reg, conn, &proto);
    expect(ret == 0, "get_registry + sync roundtrip");
    if (ret < 0)
        printf("      roundtrip %s\n", strerror(-ret));
    else {
        wp_registry_print(&reg, stdout);
        expect(reg.count >= 4, "at least 4 globals");
        expect(wp_registry_find(&reg, "wl_compositor") != NULL, "wl_compositor");
        expect(wp_registry_find(&reg, "xdg_wm_base") != NULL, "xdg_wm_base");
        g = wp_registry_find(&reg, "zwp_linux_dmabuf_v1");
        expect(g && g->version >= 4, "zwp_linux_dmabuf_v1 version >= 4");
        expect(wp_registry_find(&reg, "wp_linux_drm_syncobj_manager_v1") != NULL,
               "wp_linux_drm_syncobj_manager_v1");
    }

    wp_registry_free(&reg);
    wp_wl_conn_destroy(conn);
    free(conn);
    wp_proto_free(&proto);
    printf("%s  %d failure(s)\n", g_fail ? "RESULT FAIL" : "RESULT PASS", g_fail);
    return g_fail ? 1 : 0;
}

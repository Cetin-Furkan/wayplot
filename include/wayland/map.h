#ifndef WAYLAND_MAP_H
#define WAYLAND_MAP_H

#include <stdint.h>

enum wp_obj_kind : uint16_t {
    WP_OBJ_NONE = 0,
    WP_OBJ_DISPLAY,
    WP_OBJ_REGISTRY,
    WP_OBJ_CALLBACK,
    WP_OBJ_COMPOSITOR,
    WP_OBJ_SURFACE,
    WP_OBJ_XDG_WM,
    WP_OBJ_XDG_SURFACE,
    WP_OBJ_XDG_TOPLEVEL,
    WP_OBJ_DMABUF,
    WP_OBJ_FEEDBACK,
    WP_OBJ_SYNCOBJ_MGR,
    WP_OBJ_SYNCOBJ_SURFACE,
    WP_OBJ_SYNCOBJ_TIMELINE,
    WP_OBJ_BUFFER,
    WP_OBJ_DMABUF_PARAMS,
    WP_OBJ_OUTPUT,
    WP_OBJ_SEAT,
    WP_OBJ_POINTER,
    WP_OBJ_CURSOR_SHAPE_MGR,
    WP_OBJ_CURSOR_SHAPE_DEV,
};

struct wp_obj {
    enum wp_obj_kind kind;
    uint32_t version;
};

struct wp_map {
    struct wp_obj *v;
    uint32_t cap;
};

void wp_map_free(struct wp_map *m);
[[nodiscard]] int wp_map_set(struct wp_map *m, uint32_t id, enum wp_obj_kind kind, uint32_t version);
void wp_map_del(struct wp_map *m, uint32_t id);
[[nodiscard]] const struct wp_obj *wp_map_get(const struct wp_map *m, uint32_t id);

#endif /* WAYLAND_MAP_H */

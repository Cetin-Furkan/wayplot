#ifndef WAYLAND_REGISTRY_H
#define WAYLAND_REGISTRY_H

#include "wayland/conn.h"
#include "wayland/proto.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

struct wp_global {
    uint32_t name;
    uint32_t version;
    char iface[64];
};

struct wp_registry {
    struct wp_wl_conn *conn;
    const struct wp_proto *proto;
    uint32_t registry_id;
    uint32_t sync_id;
    bool done;
    struct wp_global *globals;
    uint32_t count;
    uint32_t cap;
};

[[nodiscard]] int wp_registry_roundtrip(struct wp_registry *r, struct wp_wl_conn *c,
                                        const struct wp_proto *proto);
[[nodiscard]] int wp_registry_handle(struct wp_registry *r, const struct wp_wl_msg *m);
void wp_registry_free(struct wp_registry *r);
void wp_registry_print(const struct wp_registry *r, FILE *out);
[[nodiscard]] const struct wp_global *wp_registry_find(const struct wp_registry *r, const char *iface);

#endif /* WAYLAND_REGISTRY_H */

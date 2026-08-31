#ifndef WAYLAND_PROTO_H
#define WAYLAND_PROTO_H

#include <stddef.h>
#include <stdint.h>

enum wp_arg_kind : uint8_t {
    WP_ARG_UINT = 1,
    WP_ARG_INT,
    WP_ARG_FIXED,
    WP_ARG_STRING,
    WP_ARG_OBJECT,
    WP_ARG_NEWID,
    WP_ARG_ARRAY,
    WP_ARG_FD,
};

struct wp_proto_arg {
    uint8_t kind;
    uint32_t name_off;
};

struct wp_proto_msg {
    uint32_t name_off;
    uint16_t opcode;
    uint16_t nargs;
    uint32_t args_off;
};

struct wp_proto_iface {
    uint32_t name_off;
    uint32_t version;
    uint16_t nrequests;
    uint16_t nevents;
    uint32_t requests_off;
    uint32_t events_off;
};

struct wp_proto {
    uint8_t *base;
    size_t size;
    uint32_t niface;
    uint32_t ifaces_off;
};

[[nodiscard]] int wp_proto_core(struct wp_proto *p);
void wp_proto_free(struct wp_proto *p);

[[nodiscard]] const char *wp_proto_str(const struct wp_proto *p, uint32_t off);
[[nodiscard]] const struct wp_proto_iface *wp_proto_iface(const struct wp_proto *p, const char *name);
[[nodiscard]] const struct wp_proto_msg *wp_proto_request(const struct wp_proto *p, const char *iface, const char *req);
[[nodiscard]] const struct wp_proto_msg *wp_proto_event(const struct wp_proto *p, const char *iface, const char *ev);

#endif /* WAYLAND_PROTO_H */

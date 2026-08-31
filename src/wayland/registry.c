#define _GNU_SOURCE
#include "wayland/registry.h"
#include "wayland/wire.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int add_global(struct wp_registry *r, uint32_t name, uint32_t version, const char *iface)
{
    if (r->count == r->cap) {
        uint32_t cap = r->cap ? r->cap * 2 : 32;
        struct wp_global *g = realloc(r->globals, cap * sizeof(*g));
        if (!g)
            return -ENOMEM;
        r->globals = g;
        r->cap = cap;
    }
    r->globals[r->count] = (struct wp_global){
        .name = name,
        .version = version,
    };
    snprintf(r->globals[r->count].iface, sizeof(r->globals[r->count].iface), "%s", iface);
    r->count++;
    return 0;
}

int wp_registry_handle(struct wp_registry *r, const struct wp_wl_msg *m)
{
    const uint32_t *raw = (const uint32_t *)m->raw;
    const struct wp_obj *obj;
    const struct wp_proto_msg *ev;

    if (!r || !r->conn || !m)
        return -EINVAL;
    obj = wp_map_get(&r->conn->map, m->obj);
    if (!obj)
        return 0;

    if (obj->kind == WP_OBJ_DISPLAY) {
        ev = wp_proto_event(r->proto, "wl_display", "error");
        if (ev && m->opcode == ev->opcode) {
            const char *msg = "?";
            uint32_t next = 0;
            uint32_t target = m->size >= 16 ? raw[2] : 0;
            uint32_t code = m->size >= 16 ? raw[3] : 0;
            if (m->size >= 20)
                (void)wp_wl_str_at(raw, m->size, 4, &msg, &next);
            fprintf(stderr, "wl_display.error obj=%u code=%u %s\n", target, code, msg);
            return -EPROTO;
        }
        ev = wp_proto_event(r->proto, "wl_display", "delete_id");
        if (ev && m->opcode == ev->opcode && m->size >= 12)
            wp_map_del(&r->conn->map, raw[2]);
        return 0;
    }

    if (obj->kind == WP_OBJ_REGISTRY) {
        ev = wp_proto_event(r->proto, "wl_registry", "global");
        if (ev && m->opcode == ev->opcode) {
            const char *iface = NULL;
            uint32_t next = 0;
            uint32_t name, ver;
            if (wp_wl_str_at(raw, m->size, 3, &iface, &next) < 0)
                return -EBADMSG;
            name = raw[2];
            if ((next + 1u) * 4u > m->size)
                return -EBADMSG;
            ver = raw[next];
            return add_global(r, name, ver, iface);
        }
        return 0;
    }

    if (obj->kind == WP_OBJ_CALLBACK && m->obj == r->sync_id) {
        ev = wp_proto_event(r->proto, "wl_callback", "done");
        if (ev && m->opcode == ev->opcode)
            r->done = true;
    }
    return 0;
}

int wp_registry_roundtrip(struct wp_registry *r, struct wp_wl_conn *c,
                          const struct wp_proto *proto)
{
    const struct wp_proto_msg *get_reg, *sync;
    uint32_t msg[3];
    uint64_t deadline;
    int ret;

    if (!r || !c || !proto)
        return -EINVAL;
    memset(r, 0, sizeof(*r));
    r->conn = c;
    r->proto = proto;

    get_reg = wp_proto_request(proto, "wl_display", "get_registry");
    sync = wp_proto_request(proto, "wl_display", "sync");
    if (!get_reg || !sync)
        return -ENOENT;

    r->registry_id = wp_wl_alloc_id(c);
    ret = wp_map_set(&c->map, r->registry_id, WP_OBJ_REGISTRY, 1);
    if (ret < 0)
        return ret;
    msg[0] = 1;
    msg[1] = (12u << 16) | get_reg->opcode;
    msg[2] = r->registry_id;
    ret = wp_wl_send(c, msg, sizeof(msg), NULL, 0);
    if (ret < 0)
        return ret;

    r->sync_id = wp_wl_alloc_id(c);
    ret = wp_map_set(&c->map, r->sync_id, WP_OBJ_CALLBACK, 1);
    if (ret < 0)
        return ret;
    msg[0] = 1;
    msg[1] = (12u << 16) | sync->opcode;
    msg[2] = r->sync_id;
    ret = wp_wl_send(c, msg, sizeof(msg), NULL, 0);
    if (ret < 0)
        return ret;

    deadline = now_ns() + 5000000000ull;
    while (!r->done) {
        struct wp_wl_msg m;
        uint64_t now = now_ns();
        uint64_t left;
        if (now >= deadline)
            return -ETIMEDOUT;
        left = deadline - now;
        ret = wp_wl_pump_wait(c, c->in_len >= 8 ? 0 : 1, left);
        if (ret == -ETIME)
            return -ETIMEDOUT;
        if (ret < 0)
            return ret;
        while (wp_wl_peek(c, &m)) {
            ret = wp_registry_handle(r, &m);
            wp_wl_consume(c);
            if (ret < 0)
                return ret;
        }
    }
    return 0;
}

void wp_registry_free(struct wp_registry *r)
{
    if (!r)
        return;
    free(r->globals);
    memset(r, 0, sizeof(*r));
}

void wp_registry_print(const struct wp_registry *r, FILE *out)
{
    uint32_t i;
    if (!r || !out)
        return;
    fprintf(out, "  name   ver  interface\n");
    for (i = 0; i < r->count; i++) {
        fprintf(out, "%6u  %4u  %s\n",
                r->globals[i].name, r->globals[i].version, r->globals[i].iface);
    }
    fprintf(out, "%u globals\n", r->count);
}

const struct wp_global *wp_registry_find(const struct wp_registry *r, const char *iface)
{
    uint32_t i;
    if (!r || !iface)
        return NULL;
    for (i = 0; i < r->count; i++) {
        if (strcmp(r->globals[i].iface, iface) == 0)
            return &r->globals[i];
    }
    return NULL;
}

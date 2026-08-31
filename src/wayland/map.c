#include "wayland/map.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

void wp_map_free(struct wp_map *m)
{
    if (!m)
        return;
    free(m->v);
    memset(m, 0, sizeof(*m));
}

int wp_map_set(struct wp_map *m, uint32_t id, enum wp_obj_kind kind, uint32_t version)
{
    if (!m || id == 0)
        return -EINVAL;
    if (id >= m->cap) {
        uint32_t cap = m->cap ? m->cap : 16;
        struct wp_obj *v;
        while (cap <= id)
            cap *= 2;
        v = realloc(m->v, cap * sizeof(*v));
        if (!v)
            return -ENOMEM;
        memset(v + m->cap, 0, (cap - m->cap) * sizeof(*v));
        m->v = v;
        m->cap = cap;
    }
    m->v[id].kind = kind;
    m->v[id].version = version;
    return 0;
}

void wp_map_del(struct wp_map *m, uint32_t id)
{
    if (!m || !m->v || id >= m->cap)
        return;
    m->v[id].kind = WP_OBJ_NONE;
    m->v[id].version = 0;
}

const struct wp_obj *wp_map_get(const struct wp_map *m, uint32_t id)
{
    if (!m || !m->v || id >= m->cap)
        return NULL;
    if (m->v[id].kind == WP_OBJ_NONE)
        return NULL;
    return &m->v[id];
}

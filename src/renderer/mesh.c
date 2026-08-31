#define _GNU_SOURCE
#include "renderer/mesh.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void wp_mesh_cpu_free(struct wp_mesh_cpu *g)
{
    if (!g)
        return;
    free(g->v);
    free(g->idx);
    memset(g, 0, sizeof(*g));
}

static void emit_face(struct wp_vn_vertex *v, uint16_t *idx, uint32_t *vi, uint32_t *ii,
                      const float n[3], const float c[3],
                      const float p0[3], const float p1[3], const float p2[3], const float p3[3])
{
    uint16_t base = (uint16_t)*vi;
    const float *p[4] = { p0, p1, p2, p3 };
    uint32_t k;
    for (k = 0; k < 4; k++) {
        v[*vi] = (struct wp_vn_vertex){
            .px = p[k][0], .py = p[k][1], .pz = p[k][2],
            .nx = n[0], .ny = n[1], .nz = n[2],
            .r = c[0], .g = c[1], .b = c[2],
        };
        (*vi)++;
    }
    /* CCW when viewed from outside (along the outward normal). */
    idx[(*ii)++] = base;
    idx[(*ii)++] = (uint16_t)(base + 1);
    idx[(*ii)++] = (uint16_t)(base + 2);
    idx[(*ii)++] = base;
    idx[(*ii)++] = (uint16_t)(base + 2);
    idx[(*ii)++] = (uint16_t)(base + 3);
}

int wp_aabb_from_vn(struct wp_aabb *b, const struct wp_vn_vertex *v, uint32_t n)
{
    uint32_t i;

    if (!b || !v || n == 0)
        return -EINVAL;
    wp_aabb_reset(b);
    for (i = 0; i < n; i++)
        wp_aabb_add(b, v[i].px, v[i].py, v[i].pz);
    return wp_aabb_ok(b) ? 0 : -EINVAL;
}

void wp_cube_cpu(struct wp_vn_vertex *v, uint16_t *idx, uint32_t *nv, uint32_t *ni)
{
    const float h = 0.5f;
    uint32_t vi = 0, ii = 0;

    emit_face(v, idx, &vi, &ii, (float[]){ 0, 0, 1 }, (float[]){ 0.91f, 0.31f, 0.31f },
              (float[]){ -h, -h, h }, (float[]){ h, -h, h }, (float[]){ h, h, h }, (float[]){ -h, h, h });
    emit_face(v, idx, &vi, &ii, (float[]){ 0, 0, -1 }, (float[]){ 0.31f, 0.72f, 0.38f },
              (float[]){ h, -h, -h }, (float[]){ -h, -h, -h }, (float[]){ -h, h, -h }, (float[]){ h, h, -h });
    emit_face(v, idx, &vi, &ii, (float[]){ 1, 0, 0 }, (float[]){ 0.31f, 0.48f, 0.91f },
              (float[]){ h, -h, h }, (float[]){ h, -h, -h }, (float[]){ h, h, -h }, (float[]){ h, h, h });
    emit_face(v, idx, &vi, &ii, (float[]){ -1, 0, 0 }, (float[]){ 0.95f, 0.82f, 0.22f },
              (float[]){ -h, -h, -h }, (float[]){ -h, -h, h }, (float[]){ -h, h, h }, (float[]){ -h, h, -h });
    emit_face(v, idx, &vi, &ii, (float[]){ 0, 1, 0 }, (float[]){ 0.28f, 0.84f, 0.84f },
              (float[]){ -h, h, h }, (float[]){ h, h, h }, (float[]){ h, h, -h }, (float[]){ -h, h, -h });
    emit_face(v, idx, &vi, &ii, (float[]){ 0, -1, 0 }, (float[]){ 0.91f, 0.38f, 0.78f },
              (float[]){ -h, -h, -h }, (float[]){ h, -h, -h }, (float[]){ h, -h, h }, (float[]){ -h, -h, h });
    *nv = vi;
    *ni = ii;
}

int wp_mesh_upload(struct wp_device *d, struct wp_mesh *m,
                   const struct wp_vn_vertex *v, uint32_t nv,
                   const uint16_t *idx, uint32_t ni)
{
    int ret;
    VkDeviceSize vbytes, ibytes;

    if (!d || !m || !v || !idx || nv == 0 || ni == 0)
        return -EINVAL;
    if (nv > WP_MESH_MAX_V || ni > WP_MESH_MAX_I)
        return -E2BIG;
    memset(m, 0, sizeof(*m));
    vbytes = (VkDeviceSize)nv * sizeof(*v);
    ibytes = (VkDeviceSize)ni * sizeof(*idx);
    ret = wp_buffer_create(d, vbytes,
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &m->vbo);
    if (ret < 0)
        return ret;
    ret = wp_buffer_upload(d, &m->vbo, v, vbytes);
    if (ret < 0) {
        wp_mesh_destroy(d, m);
        return ret;
    }
    ret = wp_buffer_create(d, ibytes,
                           VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &m->ibo);
    if (ret < 0) {
        wp_mesh_destroy(d, m);
        return ret;
    }
    ret = wp_buffer_upload(d, &m->ibo, idx, ibytes);
    if (ret < 0) {
        wp_mesh_destroy(d, m);
        return ret;
    }
    m->vertex_count = nv;
    m->index_count = ni;
    (void)wp_aabb_from_vn(&m->aabb, v, nv);
    return 0;
}

int wp_mesh_cube(struct wp_device *d, struct wp_mesh *m)
{
    struct wp_vn_vertex v[24];
    uint16_t idx[36];
    uint32_t nv = 0, ni = 0;

    if (!d || !m)
        return -EINVAL;
    wp_cube_cpu(v, idx, &nv, &ni);
    return wp_mesh_upload(d, m, v, nv, idx, ni);
}

void wp_mesh_destroy(struct wp_device *d, struct wp_mesh *m)
{
    if (!m)
        return;
    wp_buffer_destroy(d, &m->ibo);
    wp_buffer_destroy(d, &m->vbo);
    memset(m, 0, sizeof(*m));
}

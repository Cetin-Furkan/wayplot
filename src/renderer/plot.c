#define _GNU_SOURCE
#include "renderer/plot.h"

#include "helper/math3d.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void wp_plot_free(struct wp_plot *p)
{
    if (!p)
        return;
    free(p->y);
    memset(p, 0, sizeof(*p));
}

int wp_plot_resize(struct wp_plot *p, uint32_t n)
{
    float *y;

    if (!p || n < 2 || n > WP_PLOT_MAX)
        return -EINVAL;
    y = calloc(n, sizeof(float));
    if (!y)
        return -ENOMEM;
    free(p->y);
    p->y = y;
    p->n = n;
    return 0;
}

int wp_plot_parse(const void *data, size_t len, struct wp_plot *out)
{
    const char *p, *end;
    float tmp[WP_PLOT_MAX];
    uint32_t n = 0;
    int ret;

    if (!data || !out || len == 0)
        return -EINVAL;
    p = data;
    end = p + len;
    while (p < end && n < WP_PLOT_MAX) {
        char *next;
        float v;

        while (p < end && isspace((unsigned char)*p))
            p++;
        if (p >= end)
            break;
        if (*p == '#') {
            while (p < end && *p != '\n')
                p++;
            continue;
        }
        errno = 0;
        v = strtof(p, &next);
        if (next == p || errno == ERANGE || !isfinite(v))
            return -EINVAL;
        tmp[n++] = v;
        p = next;
    }
    if (n < 2)
        return -EINVAL;
    ret = wp_plot_resize(out, n);
    if (ret < 0)
        return ret;
    memcpy(out->y, tmp, (size_t)n * sizeof(float));
    return 0;
}

int wp_plot_load(const char *path, struct wp_plot *out)
{
    struct stat st;
    char *buf = NULL;
    size_t got = 0;
    ssize_t nread;
    int fd, ret;

    if (!path || !out)
        return -EINVAL;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    if (fstat(fd, &st) < 0) {
        ret = -errno;
        close(fd);
        return ret;
    }
    if (!S_ISREG(st.st_mode) || st.st_size <= 0) {
        close(fd);
        return -EINVAL;
    }
    if ((uint64_t)st.st_size > (4ull << 20)) {
        close(fd);
        return -EFBIG;
    }
    buf = malloc((size_t)st.st_size + 1u);
    if (!buf) {
        close(fd);
        return -ENOMEM;
    }
    while (got < (size_t)st.st_size) {
        nread = read(fd, buf + got, (size_t)st.st_size - got);
        if (nread < 0) {
            ret = -errno;
            free(buf);
            close(fd);
            return ret;
        }
        if (nread == 0)
            break;
        got += (size_t)nread;
    }
    close(fd);
    buf[got] = '\0';
    ret = wp_plot_parse(buf, got, out);
    free(buf);
    return ret;
}

int wp_plot_tessellate(const struct wp_plot *p, float amp, struct wp_mesh_cpu *out)
{
    uint32_t i, n, nv, ni, t;
    float sx;

    if (!p || !p->y || !out || p->n < 2)
        return -EINVAL;
    if (amp <= 0.0f)
        amp = WP_PLOT_AMP;
    n = p->n;
    nv = n * 2u;
    ni = (n - 1) * 12u; /* two sides: CULL_BACK, not CULL_NONE */
    if (nv > WP_MESH_MAX_V || ni > WP_MESH_MAX_I)
        return -E2BIG;
    memset(out, 0, sizeof(*out));
    out->v = calloc(nv, sizeof(*out->v));
    out->idx = calloc(ni, sizeof(*out->idx));
    if (!out->v || !out->idx) {
        wp_mesh_cpu_free(out);
        return -ENOMEM;
    }
    sx = 2.0f / (float)(n - 1);
    for (i = 0; i < n; i++) {
        float x = -1.0f + (float)i * sx;
        float y = p->y[i] * amp;
        float dhdx, nrm[3], tcol;
        uint32_t i0 = i > 0 ? i - 1 : i;
        uint32_t i1 = i + 1 < n ? i + 1 : i;
        float dx = (float)(i1 - i0) * sx;
        uint32_t k;

        dhdx = dx > 1e-8f ? (p->y[i1] - p->y[i0]) * amp / dx : 0.0f;
        nrm[0] = -dhdx;
        nrm[1] = 1.0f;
        nrm[2] = 0.0f;
        wp_vec3_normalize(nrm);
        tcol = p->y[i];
        if (tcol < 0.0f)
            tcol = 0.0f;
        if (tcol > 1.0f)
            tcol = 1.0f;
        for (k = 0; k < 2; k++) {
            struct wp_vn_vertex *v = &out->v[k * n + i];
            v->px = x;
            v->py = y;
            v->pz = (k == 0) ? -WP_PLOT_HALF_W : WP_PLOT_HALF_W;
            v->nx = nrm[0];
            v->ny = nrm[1];
            v->nz = nrm[2];
            v->r = 0.18f;
            v->g = 0.40f + tcol * 0.45f;
            v->b = 0.88f;
            v->u = (float)i / (float)(n - 1);
            v->v = (float)k;
        }
    }
    t = 0;
    for (i = 0; i < n - 1; i++) {
        uint16_t v00 = (uint16_t)i;
        uint16_t v10 = (uint16_t)(i + 1);
        uint16_t v01 = (uint16_t)(n + i);
        uint16_t v11 = (uint16_t)(n + i + 1);
        out->idx[t++] = v00;
        out->idx[t++] = v01;
        out->idx[t++] = v11;
        out->idx[t++] = v00;
        out->idx[t++] = v11;
        out->idx[t++] = v10;
        /* Underside: reverse winding, same verts. Looking from −Y is a front. */
        out->idx[t++] = v00;
        out->idx[t++] = v10;
        out->idx[t++] = v11;
        out->idx[t++] = v00;
        out->idx[t++] = v11;
        out->idx[t++] = v01;
    }
    out->nv = nv;
    out->ni = t;
    return 0;
}

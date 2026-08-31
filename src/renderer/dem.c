#define _GNU_SOURCE
#include "renderer/dem.h"

#include "helper/math3d.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void wp_dem_free(struct wp_dem *d)
{
    if (!d)
        return;
    free(d->h);
    memset(d, 0, sizeof(*d));
}

static int skip_pgm(const unsigned char **pp, const unsigned char *end)
{
    const unsigned char *p = *pp;

    for (;;) {
        while (p < end && isspace(*p))
            p++;
        if (p < end && *p == '#') {
            while (p < end && *p != '\n')
                p++;
            continue;
        }
        break;
    }
    *pp = p;
    return p < end ? 0 : -EINVAL;
}

static int parse_u32(const unsigned char **pp, const unsigned char *end, uint32_t *out)
{
    unsigned long v = 0;
    const unsigned char *p;

    if (skip_pgm(pp, end) < 0)
        return -EINVAL;
    p = *pp;
    if (p >= end || !isdigit(*p))
        return -EINVAL;
    while (p < end && isdigit(*p)) {
        v = v * 10u + (unsigned long)(*p - '0');
        if (v > 65535ul)
            return -E2BIG;
        p++;
    }
    *out = (uint32_t)v;
    *pp = p;
    return 0;
}

int wp_dem_parse_pgm(const void *data, size_t len, struct wp_dem *out)
{
    const unsigned char *p, *end, *bin;
    uint32_t cols, rows, maxv, i, n;
    int ret;

    if (!data || !out || len < 8)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    p = data;
    end = p + len;
    if (p[0] != 'P' || p[1] != '5')
        return -EINVAL;
    p += 2;
    if (parse_u32(&p, end, &cols) < 0 || parse_u32(&p, end, &rows) < 0 ||
        parse_u32(&p, end, &maxv) < 0)
        return -EINVAL;
    if (cols < 2 || rows < 2 || cols > WP_DEM_MAX_SIDE || rows > WP_DEM_MAX_SIDE)
        return -E2BIG;
    if (maxv == 0)
        return -EINVAL;
    if (p >= end || !isspace(*p))
        return -EINVAL;
    p++;
    n = cols * rows;
    if ((uint64_t)n > WP_MESH_MAX_V)
        return -E2BIG;
    if (maxv < 256) {
        if ((size_t)(end - p) < n)
            return -EINVAL;
        bin = p;
        out->h = calloc(n, sizeof(float));
        if (!out->h)
            return -ENOMEM;
        for (i = 0; i < n; i++)
            out->h[i] = (float)bin[i] / (float)maxv;
    } else {
        if ((size_t)(end - p) < (size_t)n * 2u)
            return -EINVAL;
        bin = p;
        out->h = calloc(n, sizeof(float));
        if (!out->h)
            return -ENOMEM;
        for (i = 0; i < n; i++) {
            uint32_t s = ((uint32_t)bin[i * 2] << 8) | (uint32_t)bin[i * 2 + 1];
            if (s > maxv)
                s = maxv;
            out->h[i] = (float)s / (float)maxv;
        }
    }
    out->cols = cols;
    out->rows = rows;
    (void)ret;
    return 0;
}

int wp_dem_load(const char *path, struct wp_dem *out)
{
    struct stat st;
    unsigned char *buf = NULL;
    size_t got = 0;
    ssize_t nread;
    int fd, ret;

    if (!path || !out)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
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
    if ((uint64_t)st.st_size > (16ull << 20)) {
        close(fd);
        return -EFBIG;
    }
    buf = malloc((size_t)st.st_size);
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
    ret = wp_dem_parse_pgm(buf, got, out);
    free(buf);
    return ret;
}

int wp_dem_tessellate(const struct wp_dem *d, float amp, struct wp_mesh_cpu *out)
{
    uint32_t i, j, cols, rows, nv, ni, t;
    float sx, sz;

    if (!d || !d->h || !out || d->cols < 2 || d->rows < 2)
        return -EINVAL;
    if (amp <= 0.0f)
        amp = WP_DEM_AMP;
    cols = d->cols;
    rows = d->rows;
    nv = cols * rows;
    ni = (cols - 1) * (rows - 1) * 6u;
    if (nv > WP_MESH_MAX_V || ni > WP_MESH_MAX_I)
        return -E2BIG;
    memset(out, 0, sizeof(*out));
    out->v = calloc(nv, sizeof(*out->v));
    out->idx = calloc(ni, sizeof(*out->idx));
    if (!out->v || !out->idx) {
        wp_mesh_cpu_free(out);
        return -ENOMEM;
    }
    sx = 2.0f / (float)(cols - 1);
    sz = 2.0f / (float)(rows - 1);
    for (j = 0; j < rows; j++) {
        for (i = 0; i < cols; i++) {
            float x = -1.0f + (float)i * sx;
            float z = -1.0f + (float)j * sz;
            float y = d->h[j * cols + i] * amp;
            float dhdx, dhdz, n[3];
            uint32_t i0 = i > 0 ? i - 1 : i;
            uint32_t i1 = i + 1 < cols ? i + 1 : i;
            uint32_t j0 = j > 0 ? j - 1 : j;
            uint32_t j1 = j + 1 < rows ? j + 1 : j;
            float dx = (float)(i1 - i0) * sx;
            float dz = (float)(j1 - j0) * sz;
            struct wp_vn_vertex *v = &out->v[j * cols + i];

            dhdx = dx > 1e-8f ? (d->h[j * cols + i1] - d->h[j * cols + i0]) * amp / dx : 0.0f;
            dhdz = dz > 1e-8f ? (d->h[j1 * cols + i] - d->h[j0 * cols + i]) * amp / dz : 0.0f;
            n[0] = -dhdx;
            n[1] = 1.0f;
            n[2] = -dhdz;
            wp_vec3_normalize(n);
            v->px = x;
            v->py = y;
            v->pz = z;
            v->nx = n[0];
            v->ny = n[1];
            v->nz = n[2];
            v->r = 0.25f + d->h[j * cols + i] * 0.55f;
            v->g = 0.45f + d->h[j * cols + i] * 0.35f;
            v->b = 0.22f;
            v->u = (float)i / (float)(cols - 1);
            v->v = (float)j / (float)(rows - 1);
        }
    }
    t = 0;
    for (j = 0; j < rows - 1; j++) {
        for (i = 0; i < cols - 1; i++) {
            uint16_t v00 = (uint16_t)(j * cols + i);
            uint16_t v10 = (uint16_t)(j * cols + i + 1);
            uint16_t v01 = (uint16_t)((j + 1) * cols + i);
            uint16_t v11 = (uint16_t)((j + 1) * cols + i + 1);
            out->idx[t++] = v00;
            out->idx[t++] = v01;
            out->idx[t++] = v11;
            out->idx[t++] = v00;
            out->idx[t++] = v11;
            out->idx[t++] = v10;
        }
    }
    out->nv = nv;
    out->ni = t;
    return 0;
}

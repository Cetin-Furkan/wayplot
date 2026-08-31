#define _GNU_SOURCE
#include "renderer/obj.h"

#include "helper/math3d.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FACE_CORNERS 32
#define LINE_MAX 4096
#define DEFAULT_R 0.82f
#define DEFAULT_G 0.82f
#define DEFAULT_B 0.82f

struct pos {
    float x, y, z;
    float r, g, b;
};

struct nrm {
    float x, y, z;
};

static int grow(void **p, uint32_t *cap, uint32_t need, size_t elem)
{
    uint32_t ncap;
    void *q;

    if (need <= *cap)
        return 0;
    ncap = *cap ? *cap : 64u;
    while (ncap < need) {
        if (ncap > (UINT32_MAX / 2u))
            return -ENOMEM;
        ncap *= 2u;
    }
    q = realloc(*p, (size_t)ncap * elem);
    if (!q)
        return -ENOMEM;
    *p = q;
    *cap = ncap;
    return 0;
}

static char *skip_ws(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

static int parse_corner(char **pp, int *vi, int *vti, int *vni)
{
    char *er;
    long v;
    char *s = skip_ws(*pp);

    *vi = *vti = *vni = 0;
    if (*s == 0)
        return -EINVAL;
    v = strtol(s, &er, 10);
    if (er == s)
        return -EINVAL;
    *vi = (int)v;
    s = er;
    if (*s != '/') {
        *pp = s;
        return 0;
    }
    s++;
    if (*s != '/' && *s != 0 && *s != ' ' && *s != '\t') {
        v = strtol(s, &er, 10);
        if (er == s)
            return -EINVAL;
        *vti = (int)v;
        s = er;
    }
    if (*s == '/') {
        s++;
        if (*s != 0 && *s != ' ' && *s != '\t') {
            v = strtol(s, &er, 10);
            if (er == s)
                return -EINVAL;
            *vni = (int)v;
            s = er;
        }
    }
    *pp = s;
    return 0;
}

static int resolve(int idx, uint32_t n)
{
    int32_t i;

    if (idx > 0)
        i = idx - 1;
    else if (idx < 0)
        i = (int32_t)n + idx;
    else
        return -1;
    if (i < 0 || (uint32_t)i >= n)
        return -1;
    return i;
}

static int emit_tri(struct wp_mesh_cpu *out, uint32_t *vcap, uint32_t *icap,
                    const struct pos *pos, uint32_t npos, const struct nrm *nrms, uint32_t nnrm,
                    int vi0, int ni0, int vi1, int ni1, int vi2, int ni2)
{
    const struct pos *p[3];
    float e1[3], e2[3], fn[3], n[3];
    int nis[3] = { ni0, ni1, ni2 };
    uint32_t k, base;
    int ret;

    if ((uint32_t)vi0 >= npos || (uint32_t)vi1 >= npos || (uint32_t)vi2 >= npos)
        return -EINVAL;
    p[0] = &pos[vi0];
    p[1] = &pos[vi1];
    p[2] = &pos[vi2];
    e1[0] = p[1]->x - p[0]->x;
    e1[1] = p[1]->y - p[0]->y;
    e1[2] = p[1]->z - p[0]->z;
    e2[0] = p[2]->x - p[0]->x;
    e2[1] = p[2]->y - p[0]->y;
    e2[2] = p[2]->z - p[0]->z;
    wp_vec3_cross(fn, e1, e2);
    if (fn[0] * fn[0] + fn[1] * fn[1] + fn[2] * fn[2] < 1e-20f)
        return 0;
    wp_vec3_normalize(fn);

    if (out->nv > WP_MESH_MAX_V - 3u)
        return -E2BIG;
    if (out->ni > WP_MESH_MAX_I - 3u)
        return -E2BIG;
    ret = grow((void **)&out->v, vcap, out->nv + 3, sizeof(*out->v));
    if (ret < 0)
        return ret;
    ret = grow((void **)&out->idx, icap, out->ni + 3, sizeof(*out->idx));
    if (ret < 0)
        return ret;

    base = out->nv;
    for (k = 0; k < 3; k++) {
        n[0] = fn[0];
        n[1] = fn[1];
        n[2] = fn[2];
        if (nis[k] >= 0 && (uint32_t)nis[k] < nnrm) {
            n[0] = nrms[nis[k]].x;
            n[1] = nrms[nis[k]].y;
            n[2] = nrms[nis[k]].z;
            if (n[0] * n[0] + n[1] * n[1] + n[2] * n[2] < 1e-20f) {
                n[0] = fn[0];
                n[1] = fn[1];
                n[2] = fn[2];
            } else {
                wp_vec3_normalize(n);
            }
        }
        out->v[out->nv++] = (struct wp_vn_vertex){
            .px = p[k]->x,
            .py = p[k]->y,
            .pz = p[k]->z,
            .nx = n[0],
            .ny = n[1],
            .nz = n[2],
            .r = p[k]->r,
            .g = p[k]->g,
            .b = p[k]->b,
        };
        out->idx[out->ni++] = (uint16_t)(base + k);
    }
    return 0;
}

int wp_obj_parse(const char *text, size_t len, struct wp_mesh_cpu *out)
{
    struct pos *pos = NULL;
    struct nrm *nrms = NULL;
    uint32_t npos = 0, cpos = 0, nnrm = 0, cnrm = 0, vcap = 0, icap = 0;
    const unsigned char *raw = (const unsigned char *)text;
    size_t off = 0;
    int ret = 0;
    char line[LINE_MAX];

    if (!text || !out)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    if (len >= 3 && raw[0] == 0xef && raw[1] == 0xbb && raw[2] == 0xbf) {
        off = 3;
        len -= 3;
        text += 3;
    }

    while (off < len) {
        const char *ls;
        char *p, *er;
        size_t n = 0, rest;
        uint32_t nc, t;
        int vi[FACE_CORNERS], vni[FACE_CORNERS];
        float f[6];
        int nf;

        ls = text + off;
        rest = len - off;
        while (n < rest && ls[n] != '\n')
            n++;
        off += n + (n < rest ? 1 : 0);
        if (n && ls[n - 1] == '\r')
            n--;
        if (n >= LINE_MAX) {
            ret = -E2BIG;
            goto fail;
        }
        memcpy(line, ls, n);
        line[n] = 0;
        p = skip_ws(line);
        if (*p == 0 || *p == '#')
            continue;

        if (p[0] == 'v' && (p[1] == ' ' || p[1] == '\t')) {
            p = skip_ws(p + 1);
            nf = 0;
            while (nf < 6) {
                f[nf] = strtof(p, &er);
                if (er == p)
                    break;
                nf++;
                p = skip_ws(er);
            }
            if (nf < 3) {
                ret = -EINVAL;
                goto fail;
            }
            ret = grow((void **)&pos, &cpos, npos + 1, sizeof(*pos));
            if (ret < 0)
                goto fail;
            pos[npos].x = f[0];
            pos[npos].y = f[1];
            pos[npos].z = f[2];
            pos[npos].r = DEFAULT_R;
            pos[npos].g = DEFAULT_G;
            pos[npos].b = DEFAULT_B;
            if (nf >= 6) {
                float r = f[3], g = f[4], b = f[5];
                if (r > 1.0f || g > 1.0f || b > 1.0f) {
                    r /= 255.0f;
                    g /= 255.0f;
                    b /= 255.0f;
                }
                pos[npos].r = r;
                pos[npos].g = g;
                pos[npos].b = b;
            }
            npos++;
            continue;
        }
        if (p[0] == 'v' && p[1] == 'n' && (p[2] == ' ' || p[2] == '\t')) {
            p = skip_ws(p + 2);
            nf = 0;
            while (nf < 3) {
                f[nf] = strtof(p, &er);
                if (er == p)
                    break;
                nf++;
                p = skip_ws(er);
            }
            if (nf < 3) {
                ret = -EINVAL;
                goto fail;
            }
            ret = grow((void **)&nrms, &cnrm, nnrm + 1, sizeof(*nrms));
            if (ret < 0)
                goto fail;
            nrms[nnrm].x = f[0];
            nrms[nnrm].y = f[1];
            nrms[nnrm].z = f[2];
            nnrm++;
            continue;
        }
        if (p[0] == 'f' && (p[1] == ' ' || p[1] == '\t')) {
            p = skip_ws(p + 1);
            nc = 0;
            while (*p && nc < FACE_CORNERS) {
                int vti = 0;
                if (parse_corner(&p, &vi[nc], &vti, &vni[nc]) < 0) {
                    ret = -EINVAL;
                    goto fail;
                }
                (void)vti;
                vi[nc] = resolve(vi[nc], npos);
                if (vi[nc] < 0) {
                    ret = -EINVAL;
                    goto fail;
                }
                if (vni[nc] != 0) {
                    vni[nc] = resolve(vni[nc], nnrm);
                    if (vni[nc] < 0) {
                        ret = -EINVAL;
                        goto fail;
                    }
                } else {
                    vni[nc] = -1;
                }
                nc++;
                p = skip_ws(p);
            }
            if (*p && nc >= FACE_CORNERS) {
                ret = -E2BIG;
                goto fail;
            }
            if (nc < 3)
                continue;
            for (t = 1; t + 1 < nc; t++) {
                ret = emit_tri(out, &vcap, &icap, pos, npos, nrms, nnrm, vi[0], vni[0], vi[t],
                               vni[t], vi[t + 1], vni[t + 1]);
                if (ret < 0)
                    goto fail;
            }
            continue;
        }
        /* vt, o, g, s, usemtl, mtllib, l, p: ignore */
    }

    free(pos);
    free(nrms);
    if (out->nv == 0 || out->ni == 0) {
        wp_mesh_cpu_free(out);
        return -EINVAL;
    }
    return 0;

fail:
    free(pos);
    free(nrms);
    wp_mesh_cpu_free(out);
    return ret;
}

int wp_obj_load(const char *path, struct wp_mesh_cpu *out)
{
    struct stat st;
    char *buf = NULL;
    ssize_t nread;
    size_t got = 0;
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
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        return -EINVAL;
    }
    if (st.st_size < 0 || (uint64_t)st.st_size > WP_OBJ_MAX_FILE) {
        close(fd);
        return -EFBIG;
    }
    if (st.st_size == 0) {
        close(fd);
        return -EINVAL;
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
    ret = wp_obj_parse(buf, got, out);
    free(buf);
    return ret;
}

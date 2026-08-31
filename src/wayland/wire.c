#include "wayland/wire.h"

#include <errno.h>
#include <string.h>

int wp_wl_str_at(const uint32_t *msg, uint16_t size, uint32_t word,
                 const char **out, uint32_t *next)
{
    uint32_t slen, padded;

    if (!msg || !out || !next)
        return -EINVAL;
    if ((word + 1u) * 4u > size)
        return -EBADMSG;
    slen = msg[word];
    if (slen == 0)
        return -EBADMSG;
    padded = (slen + 3u) & ~3u;
    if ((word + 1u) * 4u + padded > size)
        return -EBADMSG;
    *out = (const char *)&msg[word + 1];
    if ((*out)[slen - 1] != '\0')
        return -EBADMSG;
    *next = word + 1 + padded / 4;
    return 0;
}

int wp_wl_array_at(const uint32_t *msg, uint16_t size, uint32_t word,
                   const void **out, uint32_t *nbytes, uint32_t *next)
{
    uint32_t n, padded;

    if (!msg || !out || !nbytes || !next)
        return -EINVAL;
    if ((word + 1u) * 4u > size)
        return -EBADMSG;
    n = msg[word];
    padded = (n + 3u) & ~3u;
    if ((word + 1u) * 4u + padded > size)
        return -EBADMSG;
    *out = &msg[word + 1];
    *nbytes = n;
    *next = word + 1 + padded / 4;
    return 0;
}

size_t wp_wl_put_str(uint32_t *dst, const char *s)
{
    size_t len, pad;

    len = strlen(s) + 1;
    pad = (len + 3) & ~3u;
    dst[0] = (uint32_t)len;
    memcpy(&dst[1], s, len);
    if (pad > len)
        memset((char *)&dst[1] + len, 0, pad - len);
    return 1 + pad / 4;
}

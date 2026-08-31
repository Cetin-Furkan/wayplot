#ifndef WAYLAND_WIRE_H
#define WAYLAND_WIRE_H

#include <stddef.h>
#include <stdint.h>

[[nodiscard]] int wp_wl_str_at(const uint32_t *msg, uint16_t size, uint32_t word,
                               const char **out, uint32_t *next);
[[nodiscard]] int wp_wl_array_at(const uint32_t *msg, uint16_t size, uint32_t word,
                                 const void **out, uint32_t *nbytes, uint32_t *next);
size_t wp_wl_put_str(uint32_t *dst, const char *s);

#endif /* WAYLAND_WIRE_H */

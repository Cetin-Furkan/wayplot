#ifndef ENGINE_H
#define ENGINE_H

#if __STDC_VERSION__ < 202311L
#error "Khoros requires pure ISO C23 (compile with -std=c23)"
#endif

#ifndef _GNU_SOURCE
#error "_GNU_SOURCE must be defined globally by the build system"
#endif

#ifdef _STDBOOL_H
#error "<stdbool.h> is strictly banned. In C23, bool, true, and false are native language keywords."
#endif

#ifdef _STDALIGN_H
#error "<stdalign.h> is strictly banned. In C23, alignas and alignof are native language keywords."
#endif

#include <stdint.h>
#include <stddef.h>

constexpr uint32_t ENGINE_VERSION_MAJOR = 0;
constexpr uint32_t ENGINE_VERSION_MINOR = 1;
constexpr uint32_t ENGINE_VERSION_PATCH = 0;

constexpr size_t   ENGINE_CACHE_LINE_SIZE   = 64;
constexpr size_t   ENGINE_PAGE_SIZE         = 4'096;

[[nodiscard]]
bool engine_init(const char* blob_path);

[[nodiscard]]
const char* engine_get_banner(void);

#endif /* ENGINE_H */

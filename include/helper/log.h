#ifndef HELPER_LOG_H
#define HELPER_LOG_H

#include <stdio.h>

#ifdef DEBUG
#define wp_debug(...)                           \
    do {                                        \
        fprintf(stderr, "[debug] " __VA_ARGS__); \
        fflush(stderr);                         \
    } while (0)
#else
#define wp_debug(...) ((void)0)
#endif

void wp_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif /* HELPER_LOG_H */

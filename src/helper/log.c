#include "helper/log.h"

#include <stdarg.h>
#include <stdio.h>

void wp_log(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}


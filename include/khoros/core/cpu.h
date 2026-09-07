#ifndef KHOROS_CORE_CPU_H
#define KHOROS_CORE_CPU_H

#if __STDC_VERSION__ < 202311L
#error "Khoros requires pure ISO C23 (compile with -std=c23)"
#endif

#ifdef _STDBOOL_H
#error "<stdbool.h> is strictly banned. In C23, bool, true, and false are native language keywords."
#endif

#ifdef _STDALIGN_H
#error "<stdalign.h> is strictly banned. In C23, alignas and alignof are native language keywords."
#endif

#include <stdatomic.h>

/*
 * SMT-friendly pause. x86 PAUSE is ~40 cycles and does not deschedule.
 * Never nanosleep on the presentation core.
 */
static inline void khr_cpu_pause(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    atomic_thread_fence(memory_order_seq_cst);
#endif
}

#endif /* KHOROS_CORE_CPU_H */

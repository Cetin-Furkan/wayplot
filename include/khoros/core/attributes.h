#ifndef KHOROS_CORE_ATTRIBUTES_H
#define KHOROS_CORE_ATTRIBUTES_H

#if __STDC_VERSION__ < 202311L
#error "Khoros requires pure ISO C23 (compile with -std=c23)"
#endif

#ifdef _STDBOOL_H
#error "<stdbool.h> is strictly banned. In C23, bool, true, and false are native language keywords."
#endif

#ifdef _STDALIGN_H
#error "<stdalign.h> is strictly banned. In C23, alignas and alignof are native language keywords."
#endif

/*
 * C23 standard attributes.
 */
#define KHR_NODISCARD [[nodiscard]]
#define KHR_MAYBE_UNUSED [[maybe_unused]]
#define KHR_DEPRECATED [[deprecated]]

#endif /* KHOROS_CORE_ATTRIBUTES_H */

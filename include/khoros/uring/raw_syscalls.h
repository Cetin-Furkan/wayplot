#ifndef KHOROS_URING_RAW_SYSCALLS_H
#define KHOROS_URING_RAW_SYSCALLS_H

#if __STDC_VERSION__ < 202311L
#error "Khoros requires pure ISO C23 (compile with -std=c23)"
#endif

#ifdef _STDBOOL_H
#error "<stdbool.h> is strictly banned. In C23, bool, true, and false are native language keywords."
#endif

#ifdef _STDALIGN_H
#error "<stdalign.h> is strictly banned. In C23, alignas and alignof are native language keywords."
#endif

#include <stdint.h>
#include <stddef.h>
#include <linux/io_uring.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <signal.h>

/*
 * Direct, raw Linux kernel syscall wrappers for io_uring.
 * Zero liburing dependency.
 */

[[nodiscard]]
int khr_sys_io_uring_setup(uint32_t entries, struct io_uring_params* params);

[[nodiscard]]
int khr_sys_io_uring_enter(int fd, uint32_t to_submit, uint32_t min_complete,
                           uint32_t flags, sigset_t* sig);

[[nodiscard]]
int khr_sys_io_uring_enter2(int fd, uint32_t to_submit, uint32_t min_complete,
                            uint32_t flags, const void* arg, size_t argsz);

[[nodiscard]]
int khr_sys_io_uring_register(int fd, uint32_t opcode, const void* arg, uint32_t nr_args);

#endif /* KHOROS_URING_RAW_SYSCALLS_H */

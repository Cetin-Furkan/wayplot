#include "khoros/uring/raw_syscalls.h"

[[nodiscard]]
int khr_sys_io_uring_setup(uint32_t entries, struct io_uring_params* params) {
    return (int)syscall(__NR_io_uring_setup, entries, params);
}

[[nodiscard]]
int khr_sys_io_uring_enter2(int fd, uint32_t to_submit, uint32_t min_complete,
                            uint32_t flags, const void* arg, size_t argsz) {
    return (int)syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags, arg, argsz);
}

[[nodiscard]]
int khr_sys_io_uring_enter(int fd, uint32_t to_submit, uint32_t min_complete,
                           uint32_t flags, sigset_t* sig) {
    return khr_sys_io_uring_enter2(fd, to_submit, min_complete, flags, sig, (size_t)(_NSIG / 8));
}

[[nodiscard]]
int khr_sys_io_uring_register(int fd, uint32_t opcode, const void* arg, uint32_t nr_args) {
    return (int)syscall(__NR_io_uring_register, fd, opcode, arg, nr_args);
}

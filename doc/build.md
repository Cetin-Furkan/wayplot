# Build System

## Makefile

| Variable | Default |
|---|---|
| `CC` | `gcc` |
| `CFLAGS` | `-std=c23 -D_GNU_SOURCE -O3 -pthread -Wall -Wextra -Wpedantic -Werror=vla -Iinclude -MMD -MP` |
| `SAN_CFLAGS` | same with `-O1 -g3 -fsanitize=address,undefined -fno-omit-frame-pointer` |

`-pthread` is on compile and link. Topology uses pthreads.

Sources: `$(wildcard src/*.c) $(wildcard src/**/*.c)`. GNU make treats `**` as a single `*`, so this is **one subdirectory level** (`src/core/`, `src/uring/`, `src/wayland/`, `src/gfx/`). A third level (`src/a/b/c.c`) will not be picked up.

Objects land in `build/` mirroring `src/`. Header deps via `-MMD -MP`, including `build/san/**/*.d`. Without the sanitizer `.d` files, `make sanitize` will link stale objects after a header change and ASan will report phantom stack overflows.

## Targets

| Target | Action |
|---|---|
| `make` / `make all` | `build/engine` |
| `make run` | build + run; `engine_init` brings topology up and down |
| `make test` | `build/test_runner`, tee to `logs/test_logs/test_results.log` + timestamped copy |
| `make sanitize` | `build/san/test_runner_san` |
| `make lint` | banned-header / banned-library grep |
| `make clean` | `rm -rf build logs test_results.log` |

## Toolchain (this machine)

- Kernel: `7.2.0-rc7-1-cachyos-rc`
- `__STDC_VERSION__`: `202311L`
- Vulkan instance: 1.4.357 present, unused by the engine binary today (no `-lvulkan`)

## CI

`.github/workflows/ci.yml` on `ubuntu-latest`:

1. Install `gcc-14`, `libvulkan-dev`, `vulkan-tools`
2. Print `__STDC_VERSION__`
3. Same banned-library greps as `make lint`
4. Reject committed `*.o` / `*.so` / `*.a` / `engine`
5. `make all`, `make run`, `make test`

CI kernels are typically 6.x. Flags such as `IORING_SETUP_NO_SQARRAY`, `IORING_REGISTER_CLOCK`, and `IORING_ENTER_NO_IOWAIT` may be missing. The project target is CachyOS 7.2, not GitHub runners.

## Git

Atomic commits per file or tightly coupled pair. Never `git add .`. See `GITHUB.md`. Do not commit `build/`, `logs/`, binaries, or credentials.

#ifndef KHOROS_WAYLAND_WINDOW_H
#define KHOROS_WAYLAND_WINDOW_H

#if __STDC_VERSION__ < 202311L
#error "Khoros requires pure ISO C23 (compile with -std=c23)"
#endif

#ifdef _STDBOOL_H
#error "<stdbool.h> is strictly banned. In C23, bool, true, and false are native language keywords."
#endif

#ifdef _STDALIGN_H
#error "<stdalign.h> is strictly banned. In C23, alignas and alignof are native language keywords."
#endif

#include "khoros/core/attributes.h"
#include "khoros/core/topology.h"
#include "khoros/gfx/device.h"
#include "khoros/gfx/bda_arena.h"

typedef struct {
    const char* blob_path;
    const char* deck_path;
    bool        no_audio;
    uint32_t    audio_card;
    uint32_t    audio_device;
    uint32_t    stress_n;
} khr_window_options_t;

/*
 * Product window: XDG size, CSD chrome (hit list + title-bar close),
 * seat/cursor, DMA-BUF present. Runs until title-bar close,
 * xdg_toplevel.close, or SIGINT. Ring A waits in io_uring.
 */
[[nodiscard]]
bool khr_window_run_opts(khr_topology_t* topo, khr_gfx_device_t* dev,
                         khr_bda_arena_t* arena, const khr_window_options_t* opts);

[[nodiscard]]
bool khr_window_run(khr_topology_t* topo, khr_gfx_device_t* dev,
                    khr_bda_arena_t* arena, const char* blob_path,
                    const char* deck_path);

#endif /* KHOROS_WAYLAND_WINDOW_H */

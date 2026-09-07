#ifndef KHOROS_WAYLAND_DMABUF_PRESENT_H
#define KHOROS_WAYLAND_DMABUF_PRESENT_H

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
#include "khoros/core/attributes.h"
#include "khoros/wayland/client.h"
#include "khoros/wayland/dmabuf.h"
#include "khoros/wayland/syncobj.h"
#include "khoros/gfx/dmabuf.h"

/*
 * Zero-copy DMA-BUF present loop with explicit DRM syncobj synchronization
 * (ORIGINAL_REQUEST R2): the cutover from the wl_shm import-free path once
 * the compositor GPU stack cooperates. Two GPU-rendered slots; each frame is
 * painted by Vulkan directly into the exported image, committed with an
 * acquire point (our render completion) and a release point (compositor
 * done), recycled on wl_buffer.release.
 *
 * Explicit-sync plumbing (stall-free by construction — no host GPU wait on
 * any path, hot or management):
 * - One DRM timeline syncobj carries acquire points. The GPU signals the
 *   device timeline semaphore per frame; khr_dmabuf_present_sync() translates
 *   completed counter values into DRM points with TIMELINE_SIGNAL (pure
 *   counter query + ioctl, both non-blocking). The compositor waits on the
 *   named point, so a lagging bridge only delays display, never corrupts.
 * - One DRM timeline syncobj per slot carries release points, imported once
 *   each (no per-frame Wayland import churn). Slot reuse is gated by
 *   wl_buffer.release events, as in the shm loop.
 * Why not Vulkan SYNC_FD export straight into the timeline? Probed live on
 * the Xe experimental stack: timeline SYNC_FD export fails at creation, and
 * FD_TO_HANDLE(IMPORT_SYNC_FILE) on a real binary sync_file returns ENOENT.
 * The counter-query bridge uses only ioctls proven green on this box. If a
 * mature stack ever provides the direct bridge, it slots into
 * khr_dmabuf_present_sync() without touching the wire loop.
 */

constexpr uint32_t KHR_DMABUF_PRESENT_SLOTS = 2;

typedef struct {
    khr_dmabuf_slot_t gfx;      /* GPU image + view + cmd + pipeline */
    uint32_t params_id;         /* import params (created/failed match) */
    uint32_t buffer_id;         /* imported wl_buffer */
    uint32_t release_tl_id;     /* imported per-slot release timeline */
    uint32_t release_handle;    /* DRM handle of the release timeline */
    uint64_t release_point;     /* last release point named */
    bool     busy;
    bool     failed;            /* import failed: slot retired, never reused */
    bool     created;           /* created event seen */
} khr_dmabuf_pslot_t;

typedef struct {
    khr_wl_client_t* client;
    uint32_t dmabuf_id;
    uint32_t surface_id;
    uint32_t mgr_id;
    uint32_t sync_surface_id;   /* syncobj endpoint for this surface */
    uint32_t acquire_tl_id;     /* imported acquire timeline */
    uint32_t acquire_handle;    /* DRM handle of the acquire timeline */
    uint64_t last_signaled;     /* last point pushed to the DRM timeline */
    khr_dmabuf_pslot_t slots[KHR_DMABUF_PRESENT_SLOTS];
    uint32_t next_slot;
    uint32_t width;
    uint32_t height;
    uint64_t frames;
    uint32_t releases;
    uint32_t created_count;
    uint32_t failed_count;
} khr_dmabuf_present_t;

/* Bind dmabuf + syncobj manager, create the syncobj endpoint, the DRM
 * timelines (one acquire + one per slot, all imported once), and both GPU
 * slots with their wl_buffers. Needs a configured surface (attach gate). */
[[nodiscard]]
bool khr_dmabuf_present_init(khr_gfx_device_t* dev, khr_wl_client_t* client,
                             uint32_t surface_id, uint32_t w, uint32_t h,
                             khr_dmabuf_present_t* out);

/* Index of the next usable slot (not busy, not failed), or UINT32_MAX when
 * the loop must pump for releases instead of blocking. */
[[nodiscard]]
uint32_t khr_dmabuf_present_next_free(const khr_dmabuf_present_t* p);

/*
 * Render frame_no into a free slot (GPU submit signals the device acquire
 * timeline), push completed points to the DRM timeline, set acquire+release
 * points, attach + commit. Marks the slot busy. False when no slot is free
 * or any stage fails — never blocks.
 */
[[nodiscard]]
bool khr_dmabuf_present_commit_frame(khr_gfx_device_t* dev,
                                     khr_dmabuf_present_t* p,
                                     khr_bda_arena_t* arena, uint64_t frame_no);

/*
 * Acquire bridge pump: query the device timeline counter and TIMELINE_SIGNAL
 * every newly completed point. Call once per present-loop lap (and inside
 * commit_frame). Non-blocking; returns points pushed so far. Lag is safe:
 * unnamed points simply wait at the compositor until a later lap pushes.
 */
[[nodiscard]]
uint64_t khr_dmabuf_present_sync(khr_gfx_device_t* dev,
                                 khr_dmabuf_present_t* p);

/* Event consumer: wl_buffer.release frees slots; params created/failed
 * resolve imports (failed retires the slot). Returns events handled. */
uint32_t khr_dmabuf_present_consume(khr_dmabuf_present_t* p,
                                    const uint8_t* data, size_t len);

void khr_dmabuf_present_destroy(khr_gfx_device_t* dev,
                                khr_dmabuf_present_t* p);

#endif /* KHOROS_WAYLAND_DMABUF_PRESENT_H */

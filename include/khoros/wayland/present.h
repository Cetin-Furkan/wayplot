#ifndef KHOROS_WAYLAND_PRESENT_H
#define KHOROS_WAYLAND_PRESENT_H

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
#include "khoros/wayland/shm.h"

/*
 * Double-buffered shm present loop: the import-free road to the first mapped
 * window (and the fallback while DMA-BUF import matures). Two wl_buffers in
 * one sealed pool; frames commit round-robin; wl_buffer.release events free
 * slots through khr_present_consume(). Presenting without a free slot is a
 * caller-side wait (pump for releases), never a block inside this module.
 * Attach only after khr_xdg_can_attach() — the loop never checks XDG state.
 */
constexpr uint32_t KHR_PRESENT_SLOTS = 2;

typedef struct {
    uint32_t buffer_id;
    uint32_t offset;
    bool     busy;
} khr_present_slot_t;

typedef struct {
    khr_wl_client_t* client;
    khr_shm_pool_t   pool;
    uint32_t         shm_id;
    uint32_t         surface_id;
    khr_present_slot_t slots[KHR_PRESENT_SLOTS];
    uint32_t         width;
    uint32_t         height;
    uint32_t         stride;
    uint64_t         frames;
    uint32_t         next_slot;
    uint32_t         releases;
} khr_present_t;

/* Bind shm, allocate a 2-buffer pool, create both wl_buffers (all free). */
[[nodiscard]]
bool khr_present_init(khr_wl_client_t* client, uint32_t surface_id,
                      uint32_t w, uint32_t h, khr_present_t* out);

/* Mapped pixels of a slot for the caller to paint (tight ARGB8888). */
[[nodiscard]]
uint8_t* khr_present_slot_pixels(khr_present_t* p, uint32_t slot);

/* Deterministic test pattern: red/green gradient, blue = frame number.
 * Shared by tests (pixel assertions) and live runs (visible animation). */
void khr_present_paint_test(khr_present_t* p, uint8_t* pixels, uint64_t frame_no);

/* Index of the next free slot, or UINT32_MAX when both are busy. */
[[nodiscard]]
uint32_t khr_present_next_free(const khr_present_t* p);

/* attach + damage + commit one free slot; marks it busy. False if the slot
 * index is invalid or already busy — never blocks. */
[[nodiscard]]
bool khr_present_commit(khr_present_t* p, uint32_t slot);

/* Release-event consumer over a wire byte stream: frees owned buffers. */
uint32_t khr_present_consume(khr_present_t* p, const uint8_t* data, size_t len);

void khr_present_destroy(khr_present_t* p);

#endif /* KHOROS_WAYLAND_PRESENT_H */

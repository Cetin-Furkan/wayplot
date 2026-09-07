#ifndef KHOROS_WAYLAND_SHM_H
#define KHOROS_WAYLAND_SHM_H

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
#include "khoros/core/attributes.h"
#include "khoros/wayland/client.h"
#include "khoros/wayland/xdg.h"

/*
 * wl_shm CPU buffers: the import-free presentation transport. Opcodes
 * verified against wayland.xml (wl_shm.create_pool=0, pool.create_buffer=0,
 * pool.destroy=1, wl_buffer.release event=0) and wl_shm.format ARGB8888=0.
 * Used for the first mapped window and as the fallback while DMA-BUF import
 * matures on experimental stacks. The pool FD crosses as SCM_RIGHTS.
 */
constexpr uint16_t KHR_WL_SHM_CREATE_POOL          = 0;
constexpr uint16_t KHR_WL_SHM_POOL_CREATE_BUFFER   = 0;
constexpr uint16_t KHR_WL_SHM_POOL_DESTROY         = 1;
constexpr uint16_t KHR_WL_BUFFER_DESTROY           = 0;
constexpr uint16_t KHR_WL_BUFFER_EVENT_RELEASE     = 0;
constexpr uint32_t KHR_WL_SHM_FORMAT_ARGB8888      = 0;

typedef struct {
    uint32_t pool_id;
    int      fd;
    size_t   size;
    void*    addr; /* MAP_SHARED mapping, client-painted */
} khr_shm_pool_t;

/* Bind wl_shm from registry globals. */
[[nodiscard]]
bool khr_shm_bind(khr_wl_client_t* client, uint32_t* out_shm_id);

/* Sealed memfd + ftruncate + mmap + create_pool (FD via SCM_RIGHTS). */
[[nodiscard]]
bool khr_shm_pool_init(khr_wl_client_t* client, uint32_t shm_id, size_t size,
                       khr_shm_pool_t* out_pool);

/* create_buffer(offset, w, h, stride, ARGB8888) inside the pool. */
[[nodiscard]]
bool khr_shm_buffer_create(khr_wl_client_t* client, const khr_shm_pool_t* pool,
                           uint32_t offset, uint32_t w, uint32_t h,
                           uint32_t stride, uint32_t* out_buffer_id);

/* attach + damage + commit in one batched send. */
[[nodiscard]]
bool khr_shm_attach_commit(khr_wl_client_t* client, uint32_t surface_id,
                           uint32_t buffer_id, uint32_t w, uint32_t h);

[[nodiscard]]
bool khr_shm_buffer_destroy(khr_wl_client_t* client, uint32_t buffer_id);

void khr_shm_pool_destroy(khr_wl_client_t* client, khr_shm_pool_t* pool);

#endif /* KHOROS_WAYLAND_SHM_H */

#ifndef KHOROS_WAYLAND_DMABUF_H
#define KHOROS_WAYLAND_DMABUF_H

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

/*
 * zwp_linux_dmabuf_v1 zero-copy import over the raw socket. Opcodes verified
 * against linux-dmabuf-v1.xml: dmabuf.create_params=1, params.add=1,
 * params.create_immed=3 (events created=0, failed=1).
 *
 * One plane per image in this milestone (XRGB8888/ARGB8888 single-plane).
 * The plane FD travels exclusively as SCM_RIGHTS ancillary data via
 * khr_wl_client_send_with_fd: three sends (params, add+fd, create_immed),
 * because one cmsg must never cover a multi-request datagram.
 */
constexpr uint16_t KHR_DMABUF_CREATE_PARAMS          = 1;
constexpr uint16_t KHR_DMABUF_PARAMS_ADD             = 1;
constexpr uint16_t KHR_DMABUF_PARAMS_CREATE_IMMED    = 3;
constexpr uint16_t KHR_DMABUF_PARAMS_EVENT_CREATED   = 0;
constexpr uint16_t KHR_DMABUF_PARAMS_EVENT_FAILED    = 1;

/* Bind zwp_linux_dmabuf_v1 from registry globals. */
[[nodiscard]]
bool khr_dmabuf_bind(khr_wl_client_t* client, uint32_t* out_dmabuf_id);

/*
 * Import one exported DMA-BUF as a wl_buffer: create_params, add (plane 0
 * with offset/stride/modifier + FD), create_immed (w/h/fourcc/flags).
 * Returns false on any send failure; *out_buffer_id holds the wl_buffer id.
 * The full variant additionally reports the params object id so the caller
 * can match the compositor's created/failed events.
 */
[[nodiscard]]
bool khr_dmabuf_import(khr_wl_client_t* client, uint32_t dmabuf_id, int dma_fd,
                       uint32_t width, uint32_t height, uint32_t drm_format,
                       uint32_t stride, uint32_t offset, uint64_t modifier,
                       uint32_t* out_buffer_id);

[[nodiscard]]
bool khr_dmabuf_import_full(khr_wl_client_t* client, uint32_t dmabuf_id,
                            int dma_fd, uint32_t width, uint32_t height,
                            uint32_t drm_format, uint32_t stride,
                            uint32_t offset, uint64_t modifier,
                            uint32_t* out_params_id, uint32_t* out_buffer_id);

#endif /* KHOROS_WAYLAND_DMABUF_H */

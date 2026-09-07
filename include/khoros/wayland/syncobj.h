#ifndef KHOROS_WAYLAND_SYNCOBJ_H
#define KHOROS_WAYLAND_SYNCOBJ_H

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
 * wp_linux_drm_syncobj explicit synchronization over the raw socket. Opcodes
 * verified against linux-drm-syncobj-v1.xml: manager destroy=0,
 * get_surface=1, import_timeline=2; timeline destroy=0; surface destroy=0,
 * set_acquire_point=1, set_release_point=2.
 *
 * Points are 64-bit timeline values split hi/lo u32 on the wire. The syncobj
 * FD (a DRM timeline syncobj, created via DRM_IOCTL_SYNCOBJ_CREATE) travels
 * as SCM_RIGHTS on the import_timeline send only. Per-image isolation is a
 * caller contract: one timeline object per swapchain slot, destroyed on
 * retire before its replacement is imported.
 */
constexpr uint16_t KHR_SYNCOBJ_MGR_DESTROY          = 0;
constexpr uint16_t KHR_SYNCOBJ_MGR_GET_SURFACE       = 1;
constexpr uint16_t KHR_SYNCOBJ_MGR_IMPORT_TIMELINE   = 2;
constexpr uint16_t KHR_SYNCOBJ_TIMELINE_DESTROY      = 0;
constexpr uint16_t KHR_SYNCOBJ_SURFACE_DESTROY       = 0;
constexpr uint16_t KHR_SYNCOBJ_SURFACE_SET_ACQUIRE   = 1;
constexpr uint16_t KHR_SYNCOBJ_SURFACE_SET_RELEASE   = 2;

/* Bind wp_linux_drm_syncobj_manager_v1 from registry globals. */
[[nodiscard]]
bool khr_syncobj_bind(khr_wl_client_t* client, uint32_t* out_mgr_id);

/* import_timeline(new_id, fd): the FD crosses via SCM_RIGHTS. */
[[nodiscard]]
bool khr_syncobj_import_timeline(khr_wl_client_t* client, uint32_t mgr_id,
                                 int syncobj_fd, uint32_t* out_timeline_id);

/* get_surface(new_id, wl_surface): syncobj endpoint bound to a surface. */
[[nodiscard]]
bool khr_syncobj_get_surface(khr_wl_client_t* client, uint32_t mgr_id,
                             uint32_t wl_surface_id, uint32_t* out_surface_id);

/* Bind one acquire + one release point (batched, no FDs). */
[[nodiscard]]
bool khr_syncobj_set_points(khr_wl_client_t* client, uint32_t surface_id,
                            uint32_t acquire_timeline, uint64_t acquire_point,
                            uint32_t release_timeline, uint64_t release_point);

/* Retire a timeline: must precede any replacement import for the slot. */
[[nodiscard]]
bool khr_syncobj_destroy_timeline(khr_wl_client_t* client, uint32_t timeline_id);

#endif /* KHOROS_WAYLAND_SYNCOBJ_H */

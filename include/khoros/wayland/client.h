#ifndef KHOROS_WAYLAND_CLIENT_H
#define KHOROS_WAYLAND_CLIENT_H

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
#include <sys/socket.h>
#include <sys/un.h>
#include "khoros/core/attributes.h"
#include "khoros/uring/ring.h"
#include "khoros/uring/pbuf.h"
#include "khoros/wayland/wire.h"

constexpr size_t KHR_WL_MAX_GLOBALS = 128;

typedef struct {
    uint32_t name;
    char     interface[64];
    uint32_t version;
} khr_wl_global_t;

typedef struct {
    int          sock_fd;
    bool         is_direct;
    khr_uring_t* ring;
    struct sockaddr_un sun;

    khr_wl_global_t globals[KHR_WL_MAX_GLOBALS];
    uint32_t        globals_count;

    bool sync_completed;

    /* Discovered Core Wayland Protcol Globals */
    uint32_t compositor_name;
    uint32_t compositor_version;
    uint32_t xdg_wm_base_name;
    uint32_t xdg_wm_base_version;
    uint32_t shm_name;
    uint32_t shm_version;
    uint32_t seat_name;
    uint32_t seat_version;
    uint32_t syncobj_manager_name;
    uint32_t syncobj_manager_version;
    uint32_t dmabuf_name;
    uint32_t dmabuf_version;

    /* Inbound protocol stream buffer */
    alignas(64) uint8_t in_buf[16'384];

    khr_pbuf_t*  pbuf_tier0;
    khr_pbuf_t*  pbuf_tier1;
    struct msghdr recv_hdr;
    struct msghdr send_hdr;
    struct iovec  send_iov;
    bool          inbound_armed;
} khr_wl_client_t;

[[nodiscard]]
bool khr_wl_client_connect(khr_wl_client_t* client, khr_uring_t* ring, const char* override_path);

void khr_wl_client_disconnect(khr_wl_client_t* client);

[[nodiscard]]
bool khr_wl_client_roundtrip(khr_wl_client_t* client);

[[nodiscard]]
const khr_wl_global_t* khr_wl_client_find_global(const khr_wl_client_t* client, const char* interface);

void khr_wl_client_attach_pbufs(khr_wl_client_t* client, khr_pbuf_t* tier0, khr_pbuf_t* tier1);

[[nodiscard]]
bool khr_wl_client_arm_inbound(khr_wl_client_t* client);

[[nodiscard]]
bool khr_wl_client_arm_inbound_tier1(khr_wl_client_t* client);

[[nodiscard]]
bool khr_wl_client_send_skip(khr_wl_client_t* client, const void* data, size_t len);

/*
 * Non-blocking inbound consumer for the production event loop. Classifies one
 * harvested Ring A completion (pump evt_q entry): multishot PBUF packets
 * (KHR_TAG_WL_RECV / _T1) are resolved to their provided buffer, parsed as
 * Wayland wire messages into the client's registry/state, and counted.
 * Returns the number of wire messages consumed (0 for foreign tags, errors,
 * or multishot re-arm signals such as -ENOBUFS, which the caller handles by
 * re-arming with khr_wl_client_arm_inbound).
 * Buffer ownership stays with the caller: recycle via
 * khr_topology_recycle_cqe_buffer() after processing, exactly like the pump.
 * Deliberately without [[nodiscard]]: foreign tags legitimately yield 0 and
 * the caller keeps scanning the evt_q.
 */
uint32_t khr_wl_client_process_cqe(khr_wl_client_t* client, uint64_t user_data,
                                   int32_t res, uint32_t flags);

#endif /* KHOROS_WAYLAND_CLIENT_H */

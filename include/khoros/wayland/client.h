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

/* Forward declaration: the topology owns the completion queues the send path
 * must stage foreign completions into. Named struct tag lives in
 * khoros/core/topology.h; including it here would drag core threading into
 * every Wayland TU, so the client holds an opaque back-pointer instead. */
typedef struct khr_topology khr_topology_t;

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

    /* Client-allocated object IDs. IDs 1-3 are reserved (display, registry,
     * callback); allocation starts at 4. Zero means untouched: the allocator
     * self-initializes so stack-built test clients work without connect(). */
    uint32_t     next_id;

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

    /* Inbound protocol stream buffer (bootstrap roundtrip + PBUF reassembly) */
    alignas(64) uint8_t in_buf[65'536];
    size_t              in_len; /* bytes currently in in_buf */
    size_t              in_off; /* consumed prefix; leftover is [in_off, in_len) */

    uint32_t error_object;
    uint32_t error_code;
    char     error_msg[128];
    bool     display_error;

    khr_topology_t* topo; /* optional: attached owner for sound send-await */

    khr_pbuf_t*  pbuf_tier0;
    khr_pbuf_t*  pbuf_tier1;
    struct msghdr recv_hdr;
    struct msghdr send_hdr;
    struct iovec  send_iov;
    uint8_t       send_cmsg[CMSG_SPACE(sizeof(int))];
    bool          inbound_armed;
} khr_wl_client_t;

[[nodiscard]]
bool khr_wl_client_connect(khr_wl_client_t* client, khr_uring_t* ring, const char* override_path);

/* Allocate the next client-owned object ID (bind results, surfaces). */
[[nodiscard]]
uint32_t khr_wl_client_alloc_id(khr_wl_client_t* client);

void khr_wl_client_disconnect(khr_wl_client_t* client);

/*
 * Bootstrap-only synchronous roundtrip (get_registry + sync, one-shot
 * blocking receives). Call once per connection, before arm_inbound: after the
 * multishot inbound is armed, replies may land in PBUF completions instead of
 * the one-shot receive, so a mid-session roundtrip can time out on a healthy
 * connection. Steady-state sync uses an explicit sync request plus the pump.
 */
[[nodiscard]]
bool khr_wl_client_roundtrip(khr_wl_client_t* client);

[[nodiscard]]
const khr_wl_global_t* khr_wl_client_find_global(const khr_wl_client_t* client, const char* interface);

void khr_wl_client_attach_pbufs(khr_wl_client_t* client, khr_pbuf_t* tier0, khr_pbuf_t* tier1);

/*
 * Attach the owning topology. Required before any send_* call on a shared
 * ring: the send path awaits its own completion by tag and stages any foreign
 * completions (inbound packets, worker signals) into the topology's queues
 * instead of swallowing them. Without an attached topology the sends fall
 * back to consuming the next CQE blindly, which is only sound on a private
 * ring with nothing else in flight (single-threaded tests).
 */
void khr_wl_client_attach_topology(khr_wl_client_t* client, khr_topology_t* topo);

[[nodiscard]]
bool khr_wl_client_arm_inbound(khr_wl_client_t* client);

[[nodiscard]]
bool khr_wl_client_arm_inbound_tier1(khr_wl_client_t* client);

[[nodiscard]]
bool khr_wl_client_send_skip(khr_wl_client_t* client, const void* data, size_t len);

/*
 * Same as send_skip, plus one SCM_RIGHTS file descriptor on the datagram.
 * Wayland 'h'-typed arguments (DMA-BUF planes, syncobj timelines) travel
 * exclusively as ancillary data: no byte-stream slot exists for them, and a
 * multi-request datagram must never share one cmsg, so each FD-carrying
 * request gets its own send. Both send paths await their own completion by
 * tag (KHR_TAG_WL_SEND) when a topology is attached and require the exact
 * byte count, so the caller's buffer is safe to reuse on return and the
 * completion queues carry zero debt afterwards.
 */
[[nodiscard]]
bool khr_wl_client_send_with_fd(khr_wl_client_t* client, const void* data,
                                size_t len, int fd);

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

/*
 * Steady-state inbound: parse one harvested CQE into the stream stash,
 * dispatch complete Wayland messages (registry + wl_display.error), and
 * report the complete-message prefix so XDG/present consumers can walk the
 * same bytes. Sets *need_rearm on -ENOBUFS (multishot exhausted).
 * out_data/out_len are valid until the next feed/roundtrip on this client.
 */
uint32_t khr_wl_client_feed_cqe(khr_wl_client_t* client, uint64_t user_data,
                                int32_t res, uint32_t flags,
                                const uint8_t** out_data, size_t* out_len,
                                bool* need_rearm);

#endif /* KHOROS_WAYLAND_CLIENT_H */

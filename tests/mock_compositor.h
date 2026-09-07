#ifndef KHOROS_TESTS_MOCK_COMPOSITOR_H
#define KHOROS_TESTS_MOCK_COMPOSITOR_H

#if __STDC_VERSION__ < 202311L
#error "Mock Compositor requires ISO C23 (-std=c23)"
#endif

#ifdef _STDBOOL_H
#error "<stdbool.h> is banned in ISO C23"
#endif

#ifdef _STDALIGN_H
#error "<stdalign.h> is banned in ISO C23"
#endif

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <errno.h>

#include "khoros/wayland/wire.h"
#include "khoros/wayland/xdg.h"
#include "khoros/wayland/dmabuf.h"
#include "khoros/wayland/syncobj.h"
#include "khoros/wayland/shm.h"

/* Protocol Opcode Constants: core/XDG/shm opcodes live in their production
 * headers (single source of truth, XML-verified); the mock keeps only its
 * extension rake. */
constexpr uint16_t KHR_WL_SURFACE_FRAME               = 3;

constexpr uint16_t KHR_XDG_WM_BASE_DESTROY            = 0;
constexpr uint16_t KHR_XDG_WM_BASE_CREATE_POSITIONER  = 1;

constexpr uint16_t KHR_XDG_SURFACE_DESTROY            = 0;
constexpr uint16_t KHR_XDG_SURFACE_GET_POPUP          = 2;
constexpr uint16_t KHR_XDG_SURFACE_SET_WINDOW_GEOM    = 3;

constexpr uint16_t KHR_XDG_TOPLEVEL_DESTROY           = 0;

constexpr uint16_t KHR_DMABUF_DESTROY                 = 0;
constexpr uint16_t KHR_DMABUF_PARAMS_DESTROY          = 0;
constexpr uint16_t KHR_DMABUF_PARAMS_CREATE           = 2;
/* CREATE_PARAMS / PARAMS_ADD / PARAMS_CREATE_IMMED come from
 * wayland/dmabuf.h (single source of truth, XML-verified). */

/* Syncobj opcodes live in wayland/syncobj.h, wl_buffer release in
 * wayland/shm.h (single source of truth, XML-verified). */

constexpr size_t MOCK_MAX_REQUESTS = 256;
constexpr size_t MOCK_MAX_FDS      = 32;

typedef struct {
    uint32_t obj_id;
    uint16_t opcode;
    uint16_t size;
    uint8_t  payload[256];
    size_t   payload_len;
    int      fd; /* -1 if no FD passed */
} mock_wl_request_t;

typedef struct {
    uint32_t plane_idx;
    uint32_t offset;
    uint32_t stride;
    uint32_t modifier_hi;
    uint32_t modifier_lo;
    int      fd;
} mock_dmabuf_plane_t;

typedef struct {
    int server_fd;
    int client_fd;

    /* Discovered client object IDs */
    uint32_t client_registry_id;
    uint32_t client_compositor_id;
    uint32_t client_wm_base_id;
    uint32_t client_dmabuf_id;
    uint32_t client_syncobj_mgr_id;
    uint32_t client_shm_id;
    uint32_t client_seat_id;

    /* Active Wayland Entities */
    uint32_t surface_id;
    uint32_t xdg_surface_id;
    uint32_t xdg_toplevel_id;
    uint32_t syncobj_surface_id;
    uint32_t get_surface_count;
    uint32_t dmabuf_bind_count;
    uint32_t timeline_id;
    uint32_t dmabuf_params_id;
    uint32_t dmabuf_buffer_id;

    /* Presentation & Sync State */
    uint32_t commit_count;
    uint32_t attach_count;
    uint32_t last_attached_buffer;
    uint32_t shm_pool_id;
    uint32_t shm_buffer_ids[4];
    uint32_t shm_buffer_count;
    uint32_t last_ack_serial;
    uint32_t ack_configure_count;
    uint32_t last_pong_serial;
    uint32_t pong_count;

    /* DMA-BUF import state */
    mock_dmabuf_plane_t dma_planes[8];
    uint32_t            dma_plane_count;
    int32_t             dma_width;
    int32_t             dma_height;
    uint32_t            dma_format;
    uint32_t            dma_flags;

    /* DRM Syncobj state */
    int      syncobj_timeline_fd;
    uint32_t acquire_timeline_id;
    uint64_t acquire_point;
    uint32_t release_timeline_id;
    uint64_t release_point;
    uint32_t timeline_destroy_count;
    /* Live imported timeline ids (per-image slots): destroy matches any of
     * these, not just the most recent import. */
    uint32_t timeline_ids[8];
    uint32_t timeline_id_count;

    /* Request Journal */
    mock_wl_request_t requests[MOCK_MAX_REQUESTS];
    uint32_t          request_count;

    /* Tracked FDs to close on destruction */
    int      received_fds[MOCK_MAX_FDS];
    uint32_t received_fd_count;
} mock_compositor_t;

[[nodiscard]]
static inline bool mock_compositor_init(mock_compositor_t* comp) {
    *comp = (mock_compositor_t){
        .server_fd = -1,
        .client_fd = -1,
        .syncobj_timeline_fd = -1,
    };

    int sv[2] = { -1, -1 };
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) {
        return false;
    }
    comp->server_fd = sv[0];
    comp->client_fd = sv[1];

    /* Set 100ms receive timeout on server_fd to prevent deadlock during drain */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 100'000 };
    setsockopt(comp->server_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    return true;
}

static inline void mock_compositor_destroy(mock_compositor_t* comp) {
    if (comp->server_fd >= 0) {
        close(comp->server_fd);
        comp->server_fd = -1;
    }
    if (comp->client_fd >= 0) {
        close(comp->client_fd);
        comp->client_fd = -1;
    }
    if (comp->syncobj_timeline_fd >= 0) {
        close(comp->syncobj_timeline_fd);
        comp->syncobj_timeline_fd = -1;
    }
    for (uint32_t i = 0; i < comp->received_fd_count; i++) {
        if (comp->received_fds[i] >= 0) {
            close(comp->received_fds[i]);
            comp->received_fds[i] = -1;
        }
    }
    comp->received_fd_count = 0;
}

[[nodiscard]]
static inline bool mock_compositor_send_raw(mock_compositor_t* comp, const void* data, size_t len) {
    if (comp->server_fd < 0 || data == nullptr || len == 0) {
        return false;
    }
    ssize_t sent = send(comp->server_fd, data, len, MSG_NOSIGNAL);
    return (sent == (ssize_t)len);
}

[[nodiscard]]
static inline bool mock_compositor_send_globals(mock_compositor_t* comp) {
    khr_wl_msg_buf_t buf = {};
    khr_wl_buf_init(&buf);

    struct {
        uint32_t name;
        const char* interface;
        uint32_t version;
    } globals[] = {
        { 1, "wl_compositor", 4 },
        { 2, "xdg_wm_base", 3 },
        { 3, "zwp_linux_dmabuf_v1", 4 },
        { 4, "wp_linux_drm_syncobj_manager_v1", 1 },
        { 5, "wl_shm", 1 },
        { 6, "wl_seat", 7 },
    };

    for (size_t i = 0; i < sizeof(globals)/sizeof(globals[0]); i++) {
        uint32_t s_len = (uint32_t)strlen(globals[i].interface) + 1;
        uint32_t s_pad = khr_wl_pad4(s_len);
        uint16_t msg_size = (uint16_t)(8 + 4 + 4 + s_pad + 4);

        if (!khr_wl_encode_header(&buf, KHR_WL_REGISTRY_ID, KHR_WL_REGISTRY_EVENT_GLOBAL, msg_size)) return false;
        if (!khr_wl_encode_u32(&buf, globals[i].name)) return false;
        if (!khr_wl_encode_string(&buf, globals[i].interface)) return false;
        if (!khr_wl_encode_u32(&buf, globals[i].version)) return false;
    }

    return mock_compositor_send_raw(comp, buf.data, buf.size);
}

[[nodiscard]]
static inline bool mock_compositor_send_sync_done(mock_compositor_t* comp, uint32_t callback_id, uint32_t serial) {
    khr_wl_msg_buf_t buf = {};
    khr_wl_buf_init(&buf);
    if (!khr_wl_encode_header(&buf, callback_id, KHR_WL_CALLBACK_EVENT_DONE, 12)) return false;
    if (!khr_wl_encode_u32(&buf, serial)) return false;
    return mock_compositor_send_raw(comp, buf.data, buf.size);
}

[[nodiscard]]
static inline bool mock_compositor_send_xdg_configure(mock_compositor_t* comp,
                                                      uint32_t toplevel_id,
                                                      uint32_t xdg_surface_id,
                                                      int32_t width,
                                                      int32_t height,
                                                      uint32_t serial) {
    khr_wl_msg_buf_t buf = {};
    khr_wl_buf_init(&buf);

    /* 1. xdg_toplevel.configure(width, height, states array) */
    /* Array format: uint32_t byte_length, followed by uint32_t elements */
    if (!khr_wl_encode_header(&buf, toplevel_id, KHR_XDG_TOPLEVEL_EVENT_CONFIGURE, 8 + 4 + 4 + 4)) return false;
    if (!khr_wl_encode_i32(&buf, width)) return false;
    if (!khr_wl_encode_i32(&buf, height)) return false;
    if (!khr_wl_encode_u32(&buf, 0)) return false; /* 0 states */

    /* 2. xdg_surface.configure(serial) */
    if (!khr_wl_encode_header(&buf, xdg_surface_id, KHR_XDG_SURFACE_EVENT_CONFIGURE, 12)) return false;
    if (!khr_wl_encode_u32(&buf, serial)) return false;

    return mock_compositor_send_raw(comp, buf.data, buf.size);
}

[[nodiscard]]
static inline bool mock_compositor_send_ping(mock_compositor_t* comp, uint32_t wm_base_id, uint32_t serial) {
    khr_wl_msg_buf_t buf = {};
    khr_wl_buf_init(&buf);
    if (!khr_wl_encode_header(&buf, wm_base_id, KHR_XDG_WM_BASE_EVENT_PING, 12)) return false;
    if (!khr_wl_encode_u32(&buf, serial)) return false;
    return mock_compositor_send_raw(comp, buf.data, buf.size);
}

[[nodiscard]]
static inline bool mock_compositor_send_buffer_release(mock_compositor_t* comp, uint32_t buffer_id) {
    khr_wl_msg_buf_t buf = {};
    khr_wl_buf_init(&buf);
    if (!khr_wl_encode_header(&buf, buffer_id, KHR_WL_BUFFER_EVENT_RELEASE, 8)) return false;
    return mock_compositor_send_raw(comp, buf.data, buf.size);
}

/* zwp_linux_buffer_params_v1.created(new_id buffer): sent some time after
 * create_immed; the buffer is usable immediately, created only confirms. */
[[nodiscard]]
static inline bool mock_compositor_send_dmabuf_created(mock_compositor_t* comp,
                                                      uint32_t params_id,
                                                      uint32_t buffer_id) {
    khr_wl_msg_buf_t buf = {};
    khr_wl_buf_init(&buf);
    if (!khr_wl_encode_header(&buf, params_id, KHR_DMABUF_PARAMS_EVENT_CREATED, 12)) return false;
    if (!khr_wl_encode_u32(&buf, buffer_id)) return false;
    return mock_compositor_send_raw(comp, buf.data, buf.size);
}

/* zwp_linux_buffer_params_v1.failed: the import died (bad format, modifier,
 * stride, or GPU-stack refusal). No arguments; the params object is done. */
[[nodiscard]]
static inline bool mock_compositor_send_dmabuf_failed(mock_compositor_t* comp,
                                                     uint32_t params_id) {
    khr_wl_msg_buf_t buf = {};
    khr_wl_buf_init(&buf);
    if (!khr_wl_encode_header(&buf, params_id, KHR_DMABUF_PARAMS_EVENT_FAILED, 8)) return false;
    return mock_compositor_send_raw(comp, buf.data, buf.size);
}

/*
 * Process a single decoded wire message and update mock compositor state
 */
static inline void mock_compositor_process_message(mock_compositor_t* comp,
                                                   const khr_wl_msg_header_t* hdr,
                                                   const uint8_t* payload,
                                                   size_t payload_len,
                                                   int received_fd) {
    /* Record in journal */
    if (comp->request_count < MOCK_MAX_REQUESTS) {
        mock_wl_request_t* req = &comp->requests[comp->request_count++];
        req->obj_id = hdr->object_id;
        req->opcode = hdr->opcode;
        req->size = hdr->size;
        req->payload_len = (payload_len <= sizeof(req->payload)) ? payload_len : sizeof(req->payload);
        if (payload != nullptr && req->payload_len > 0) {
            memcpy(req->payload, payload, req->payload_len);
        }
        req->fd = received_fd;
    }

    if (received_fd >= 0 && comp->received_fd_count < MOCK_MAX_FDS) {
        comp->received_fds[comp->received_fd_count++] = received_fd;
    }

    /* 1. wl_registry.bind */
    if (hdr->object_id == KHR_WL_REGISTRY_ID && hdr->opcode == KHR_WL_REGISTRY_BIND) {
        if (payload_len >= 12) {
            size_t off = 0;
            uint32_t name = 0;
            const char* iface = nullptr;
            uint32_t iface_len = 0;
            uint32_t version = 0;
            uint32_t new_id = 0;

            if (khr_wl_decode_u32(payload, payload_len, &off, &name) &&
                khr_wl_decode_string(payload, payload_len, &off, &iface, &iface_len) &&
                khr_wl_decode_u32(payload, payload_len, &off, &version) &&
                khr_wl_decode_u32(payload, payload_len, &off, &new_id)) {
                if (strcmp(iface, "wl_compositor") == 0) comp->client_compositor_id = new_id;
                else if (strcmp(iface, "xdg_wm_base") == 0) comp->client_wm_base_id = new_id;
                else if (strcmp(iface, "zwp_linux_dmabuf_v1") == 0) {
                    comp->client_dmabuf_id = new_id;
                    comp->dmabuf_bind_count++;
                } else if (strcmp(iface, "wp_linux_drm_syncobj_manager_v1") == 0) {
                    comp->client_syncobj_mgr_id = new_id;
                }
                else if (strcmp(iface, "wl_shm") == 0) comp->client_shm_id = new_id;
                else if (strcmp(iface, "wl_seat") == 0) comp->client_seat_id = new_id;
            }
        }
        return;
    }

    /* 2. wl_compositor.create_surface */
    if (comp->client_compositor_id > 0 && hdr->object_id == comp->client_compositor_id && hdr->opcode == 0) {
        if (payload_len >= 4) {
            size_t off = 0;
            (void)khr_wl_decode_u32(payload, payload_len, &off, &comp->surface_id);
        }
        return;
    }

    /* 3. xdg_wm_base.get_xdg_surface */
    if (comp->client_wm_base_id > 0 && hdr->object_id == comp->client_wm_base_id && hdr->opcode == KHR_XDG_WM_BASE_GET_XDG_SURFACE) {
        if (payload_len >= 8) {
            size_t off = 0;
            (void)khr_wl_decode_u32(payload, payload_len, &off, &comp->xdg_surface_id);
        }
        return;
    }

    /* 4. xdg_wm_base.pong */
    if (comp->client_wm_base_id > 0 && hdr->object_id == comp->client_wm_base_id && hdr->opcode == KHR_XDG_WM_BASE_PONG) {
        if (payload_len >= 4) {
            size_t off = 0;
            (void)khr_wl_decode_u32(payload, payload_len, &off, &comp->last_pong_serial);
            comp->pong_count++;
        }
        return;
    }

    /* 5. xdg_surface.get_toplevel */
    if (comp->xdg_surface_id > 0 && hdr->object_id == comp->xdg_surface_id && hdr->opcode == KHR_XDG_SURFACE_GET_TOPLEVEL) {
        if (payload_len >= 4) {
            size_t off = 0;
            (void)khr_wl_decode_u32(payload, payload_len, &off, &comp->xdg_toplevel_id);
        }
        return;
    }

    /* 6. xdg_surface.ack_configure */
    if (comp->xdg_surface_id > 0 && hdr->object_id == comp->xdg_surface_id && hdr->opcode == KHR_XDG_SURFACE_ACK_CONFIGURE) {
        if (payload_len >= 4) {
            size_t off = 0;
            (void)khr_wl_decode_u32(payload, payload_len, &off, &comp->last_ack_serial);
            comp->ack_configure_count++;
        }
        return;
    }

    /* 6b. wl_shm.create_pool */
    if (comp->client_shm_id > 0 && hdr->object_id == comp->client_shm_id && hdr->opcode == 0) {
        if (payload_len >= 4) {
            size_t off = 0;
            (void)khr_wl_decode_u32(payload, payload_len, &off, &comp->shm_pool_id);
        }
        return;
    }

    /* 6c. wl_shm_pool.create_buffer */
    if (comp->shm_pool_id > 0 && hdr->object_id == comp->shm_pool_id && hdr->opcode == 0) {
        if (payload_len >= 20 && comp->shm_buffer_count < 4) {
            size_t off = 0;
            uint32_t new_id = 0;
            if (khr_wl_decode_u32(payload, payload_len, &off, &new_id) && new_id > 0) {
                comp->shm_buffer_ids[comp->shm_buffer_count++] = new_id;
            }
        }
        return;
    }

    /* 7. wl_surface.attach */
    if (comp->surface_id > 0 && hdr->object_id == comp->surface_id && hdr->opcode == KHR_WL_SURFACE_ATTACH) {
        if (payload_len >= 4) {
            size_t off = 0;
            (void)khr_wl_decode_u32(payload, payload_len, &off, &comp->last_attached_buffer);
            comp->attach_count++;
        }
        return;
    }

    /* 8. wl_surface.commit */
    if (comp->surface_id > 0 && hdr->object_id == comp->surface_id && hdr->opcode == KHR_WL_SURFACE_COMMIT) {
        comp->commit_count++;
        return;
    }

    /* 9. zwp_linux_dmabuf_v1.create_params */
    if (comp->client_dmabuf_id > 0 && hdr->object_id == comp->client_dmabuf_id && hdr->opcode == KHR_DMABUF_CREATE_PARAMS) {
        if (payload_len >= 4) {
            size_t off = 0;
            (void)khr_wl_decode_u32(payload, payload_len, &off, &comp->dmabuf_params_id);
        }
        return;
    }

    /* 10. zwp_linux_buffer_params_v1.add */
    if (comp->dmabuf_params_id > 0 && hdr->object_id == comp->dmabuf_params_id && hdr->opcode == KHR_DMABUF_PARAMS_ADD) {
        if (payload_len >= 20 && comp->dma_plane_count < 8) {
            size_t off = 0;
            mock_dmabuf_plane_t* p = &comp->dma_planes[comp->dma_plane_count++];
            p->fd = received_fd;
            (void)khr_wl_decode_u32(payload, payload_len, &off, &p->plane_idx);
            (void)khr_wl_decode_u32(payload, payload_len, &off, &p->offset);
            (void)khr_wl_decode_u32(payload, payload_len, &off, &p->stride);
            (void)khr_wl_decode_u32(payload, payload_len, &off, &p->modifier_hi);
            (void)khr_wl_decode_u32(payload, payload_len, &off, &p->modifier_lo);
        }
        return;
    }

    /* 11. zwp_linux_buffer_params_v1.create_immed */
    if (comp->dmabuf_params_id > 0 && hdr->object_id == comp->dmabuf_params_id && hdr->opcode == KHR_DMABUF_PARAMS_CREATE_IMMED) {
        if (payload_len >= 20) {
            size_t off = 0;
            (void)khr_wl_decode_u32(payload, payload_len, &off, &comp->dmabuf_buffer_id);
            (void)khr_wl_decode_i32(payload, payload_len, &off, &comp->dma_width);
            (void)khr_wl_decode_i32(payload, payload_len, &off, &comp->dma_height);
            (void)khr_wl_decode_u32(payload, payload_len, &off, &comp->dma_format);
            (void)khr_wl_decode_u32(payload, payload_len, &off, &comp->dma_flags);
        }
        return;
    }

    /* 12. wp_linux_drm_syncobj_manager_v1.import_timeline */
    if (comp->client_syncobj_mgr_id > 0 && hdr->object_id == comp->client_syncobj_mgr_id && hdr->opcode == KHR_SYNCOBJ_MGR_IMPORT_TIMELINE) {
        if (payload_len >= 4) {
            size_t off = 0;
            uint32_t new_id = 0;
            if (khr_wl_decode_u32(payload, payload_len, &off, &new_id) && new_id > 0) {
                comp->timeline_id = new_id;
                if (comp->timeline_id_count < 8) {
                    comp->timeline_ids[comp->timeline_id_count++] = new_id;
                }
            }
            comp->syncobj_timeline_fd = received_fd;
        }
        return;
    }

    /* 13. wp_linux_drm_syncobj_manager_v1.get_surface */
    if (comp->client_syncobj_mgr_id > 0 && hdr->object_id == comp->client_syncobj_mgr_id && hdr->opcode == KHR_SYNCOBJ_MGR_GET_SURFACE) {
        comp->get_surface_count++;
        if (payload_len >= 8) {
            size_t off = 0;
            (void)khr_wl_decode_u32(payload, payload_len, &off, &comp->syncobj_surface_id);
        }
        return;
    }

    /* 14. wp_linux_drm_syncobj_surface_v1.set_acquire_point */
    if (comp->syncobj_surface_id > 0 && hdr->object_id == comp->syncobj_surface_id && hdr->opcode == KHR_SYNCOBJ_SURFACE_SET_ACQUIRE) {
        if (payload_len >= 12) {
            size_t off = 0;
            uint32_t t_id = 0;
            uint32_t p_hi = 0;
            uint32_t p_lo = 0;
            if (khr_wl_decode_u32(payload, payload_len, &off, &t_id) &&
                khr_wl_decode_u32(payload, payload_len, &off, &p_hi) &&
                khr_wl_decode_u32(payload, payload_len, &off, &p_lo)) {
                comp->acquire_timeline_id = t_id;
                comp->acquire_point = ((uint64_t)p_hi << 32) | (uint64_t)p_lo;
            }
        }
        return;
    }

    /* 15. wp_linux_drm_syncobj_surface_v1.set_release_point */
    if (comp->syncobj_surface_id > 0 && hdr->object_id == comp->syncobj_surface_id && hdr->opcode == KHR_SYNCOBJ_SURFACE_SET_RELEASE) {
        if (payload_len >= 12) {
            size_t off = 0;
            uint32_t t_id = 0;
            uint32_t p_hi = 0;
            uint32_t p_lo = 0;
            if (khr_wl_decode_u32(payload, payload_len, &off, &t_id) &&
                khr_wl_decode_u32(payload, payload_len, &off, &p_hi) &&
                khr_wl_decode_u32(payload, payload_len, &off, &p_lo)) {
                comp->release_timeline_id = t_id;
                comp->release_point = ((uint64_t)p_hi << 32) | (uint64_t)p_lo;
            }
        }
        return;
    }

    /* 16. wp_linux_drm_syncobj_timeline_v1.destroy (Opcode 0): matches any
     * live imported id; consumed on destroy so double-destroy is visible. */
    if (hdr->opcode == KHR_SYNCOBJ_TIMELINE_DESTROY) {
        for (uint32_t i = 0; i < comp->timeline_id_count; i++) {
            if (comp->timeline_ids[i] == hdr->object_id && hdr->object_id > 0) {
                comp->timeline_ids[i] = 0;
                comp->timeline_destroy_count++;
                return;
            }
        }
    }
}

/*
 * Drain pending requests from the client socket, handling SCM_RIGHTS file descriptors.
 */
static inline uint32_t mock_compositor_drain(mock_compositor_t* comp) {
    uint32_t new_msgs = 0;
    uint8_t buffer[8'192] = {};
    uint8_t cmsg_buf[CMSG_SPACE(sizeof(int) * 4)] = {};

    struct iovec iov = {
        .iov_base = buffer,
        .iov_len = sizeof(buffer),
    };

    while (true) {
        struct msghdr msg = {
            .msg_iov = &iov,
            .msg_iovlen = 1,
            .msg_control = cmsg_buf,
            .msg_controllen = sizeof(cmsg_buf),
        };

        ssize_t n = recvmsg(comp->server_fd, &msg, MSG_DONTWAIT);
        if (n <= 0) {
            break;
        }

        /* Extract SCM_RIGHTS FD if present */
        int received_fd = -1;
        for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
                int* fds = (int*)CMSG_DATA(cmsg);
                size_t num_fds = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                if (num_fds > 0) {
                    received_fd = fds[0];
                }
                break;
            }
        }

        /* FD ownership across a coalesced stream batch: like libwayland, the
         * received FD belongs to the first message that actually declares an
         * 'h' argument (dmabuf add, syncobj import) — never blindly to the
         * batch head. Shadow IDs learned mid-batch (create_params precedes
         * its add) so ordering inside one recvmsg still resolves. */
        size_t fd_target = (size_t)-1;
        {
            size_t scan = 0;
            /* Shadow the ID learning that process_message performs, so a
             * fully-coalesced bind+params+add batch still resolves: the bind
             * teaches the dmabuf/syncobj IDs, create_params teaches params. */
            uint32_t shadow_dmabuf = comp->client_dmabuf_id;
            uint32_t shadow_mgr = comp->client_syncobj_mgr_id;
            uint32_t shadow_params = comp->dmabuf_params_id;
            while (scan + 8 <= (size_t)n) {
                khr_wl_msg_header_t shdr = {};
                if (!khr_wl_decode_header(buffer + scan, (size_t)n - scan, &shdr)) {
                    break;
                }
                if (shdr.size < 8 || scan + shdr.size > (size_t)n) {
                    break;
                }
                if (shdr.object_id == KHR_WL_REGISTRY_ID &&
                    shdr.opcode == KHR_WL_REGISTRY_BIND) {
                    size_t off = 0;
                    uint32_t name = 0;
                    const char* iface = nullptr;
                    uint32_t iface_len = 0;
                    uint32_t version = 0;
                    uint32_t new_id = 0;
                    if (khr_wl_decode_u32(buffer + scan + 8, shdr.size - 8, &off, &name) &&
                        khr_wl_decode_string(buffer + scan + 8, shdr.size - 8, &off, &iface, &iface_len) &&
                        khr_wl_decode_u32(buffer + scan + 8, shdr.size - 8, &off, &version) &&
                        khr_wl_decode_u32(buffer + scan + 8, shdr.size - 8, &off, &new_id)) {
                        (void)name;
                        (void)version;
                        if (iface != nullptr && strcmp(iface, "zwp_linux_dmabuf_v1") == 0) {
                            shadow_dmabuf = new_id;
                        } else if (iface != nullptr &&
                                   strcmp(iface, "wp_linux_drm_syncobj_manager_v1") == 0) {
                            shadow_mgr = new_id;
                        }
                    }
                }
                if (shadow_dmabuf > 0 && shdr.object_id == shadow_dmabuf &&
                    shdr.opcode == KHR_DMABUF_CREATE_PARAMS && shdr.size >= 12) {
                    size_t off = 0;
                    uint32_t new_id = 0;
                    if (khr_wl_decode_u32(buffer + scan + 8, shdr.size - 8, &off, &new_id)) {
                        shadow_params = new_id;
                    }
                }
                bool takes_fd =
                    (shadow_params > 0 && shdr.object_id == shadow_params &&
                     shdr.opcode == KHR_DMABUF_PARAMS_ADD) ||
                    (shadow_mgr > 0 && shdr.object_id == shadow_mgr &&
                     shdr.opcode == KHR_SYNCOBJ_MGR_IMPORT_TIMELINE);
                if (takes_fd) {
                    fd_target = scan;
                    break;
                }
                scan += shdr.size;
            }
            if (fd_target == (size_t)-1) {
                fd_target = 0; /* legacy: no FD-message present */
            }
        }

        /* Process all concatenated Wayland messages in this read batch */
        size_t offset = 0;
        while (offset + 8 <= (size_t)n) {
            khr_wl_msg_header_t hdr = {};
            if (!khr_wl_decode_header(buffer + offset, (size_t)n - offset, &hdr)) {
                break;
            }
            if (hdr.size < 8 || offset + hdr.size > (size_t)n) {
                break;
            }

            const uint8_t* payload = buffer + offset + 8;
            size_t payload_len = hdr.size - 8;

            int msg_fd = (offset == fd_target) ? received_fd : -1;

            mock_compositor_process_message(comp, &hdr, payload, payload_len, msg_fd);
            new_msgs++;
            offset += hdr.size;
        }
    }

    return new_msgs;
}

[[nodiscard]]
static inline bool mock_compositor_has_request(const mock_compositor_t* comp, uint32_t obj_id, uint16_t opcode) {
    for (uint32_t i = 0; i < comp->request_count; i++) {
        if (comp->requests[i].obj_id == obj_id && comp->requests[i].opcode == opcode) {
            return true;
        }
    }
    return false;
}

[[nodiscard]]
static inline const mock_wl_request_t* mock_compositor_find_request(const mock_compositor_t* comp, uint32_t obj_id, uint16_t opcode) {
    for (uint32_t i = 0; i < comp->request_count; i++) {
        if (comp->requests[i].obj_id == obj_id && comp->requests[i].opcode == opcode) {
            return &comp->requests[i];
        }
    }
    return nullptr;
}

/*
 * Helper for sending client-side message with SCM_RIGHTS FD
 */
[[nodiscard]]
static inline bool mock_client_send_msg_with_fd(int sock_fd, const void* data, size_t len, int pass_fd) {
    struct iovec iov = {
        .iov_base = (void*)data,
        .iov_len = len,
    };

    uint8_t cmsg_buf[CMSG_SPACE(sizeof(int))] = {};
    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
    };

    if (pass_fd >= 0) {
        msg.msg_control = cmsg_buf;
        msg.msg_controllen = sizeof(cmsg_buf);
        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), &pass_fd, sizeof(int));
    }

    ssize_t sent = sendmsg(sock_fd, &msg, MSG_NOSIGNAL);
    return (sent == (ssize_t)len);
}

#endif /* KHOROS_TESTS_MOCK_COMPOSITOR_H */

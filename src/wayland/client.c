#include "khoros/wayland/client.h"
#include "khoros/core/topology.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>

[[nodiscard]]
bool khr_wl_client_connect(khr_wl_client_t* client, khr_uring_t* ring, const char* override_path) {
    if (client == nullptr || ring == nullptr) {
        return false;
    }
    *client = (khr_wl_client_t){
        .sock_fd = -1,
        .is_direct = false,
        .ring = ring,
        .next_id = KHR_WL_CALLBACK_ID + 1U,
    };

    struct sockaddr_un sun = { .sun_family = AF_UNIX };
    if (override_path != nullptr) {
        int len = snprintf(sun.sun_path, sizeof(sun.sun_path), "%s", override_path);
        if (len < 0 || (size_t)len >= sizeof(sun.sun_path)) {
            return false;
        }
    } else {
        const char* xdg = getenv("XDG_RUNTIME_DIR");
        const char* disp = getenv("WAYLAND_DISPLAY");
        if (xdg == nullptr) {
            xdg = "/run/user/1000";
        }
        if (disp == nullptr) {
            disp = "wayland-0";
        }
        int len = snprintf(sun.sun_path, sizeof(sun.sun_path), "%s/%s", xdg, disp);
        if (len < 0 || (size_t)len >= sizeof(sun.sun_path)) {
            return false;
        }
    }
    client->sun = sun;

    /* Ensure ring has registered files table (fallback register sparse if needed) */
    if (!ring->files_registered) {
        (void)khr_uring_register_files_sparse(ring, KHR_DEFAULT_SPARSE_FILES);
    }

    if (ring->files_registered && ring->registered_files_count > KHR_DIRECT_SLOT_WAYLAND) {
        /* Atomic direct descriptor socket + connect linked via IOSQE_IO_LINK */
        struct io_uring_sqe* sock_sqe = khr_uring_prep_socket(
            ring, AF_UNIX, SOCK_STREAM, 0, KHR_DIRECT_SLOT_WAYLAND, true, KHR_TAG_WL_SOCKET);
        if (sock_sqe == nullptr) {
            return false;
        }
        sock_sqe->flags |= IOSQE_IO_LINK;

        struct io_uring_sqe* conn_sqe = khr_uring_prep_connect(
            ring, (int)KHR_DIRECT_SLOT_WAYLAND, (struct sockaddr*)&client->sun,
            sizeof(client->sun), true, KHR_TAG_WL_CONNECT);
        if (conn_sqe == nullptr) {
            return false;
        }

        if (khr_uring_submit(ring, 2) < 0) {
            return false;
        }

        int sock_res = -1;
        int conn_res = -1;
        uint32_t reaped = 0;
        while (reaped < 2) {
            struct io_uring_cqe* cqe = nullptr;
            if (!khr_uring_wait_cqe_timeout(ring, &cqe, 2'000) || cqe == nullptr) {
                return false;
            }
            uint64_t ud = cqe->user_data;
            int res = cqe->res;
            khr_uring_cqe_seen(ring, cqe);
            if (ud == KHR_TAG_WL_SOCKET) {
                sock_res = res;
                reaped++;
            } else if (ud == KHR_TAG_WL_CONNECT) {
                conn_res = res;
                reaped++;
            }
        }

        if (sock_res < 0) {
            return false;
        }

        if (conn_res < 0) {
            /* Socket created in direct slot but connect failed; close direct slot */
            struct io_uring_sqe* close_sqe = khr_uring_prep_close(
                ring, (int)KHR_DIRECT_SLOT_WAYLAND, true, KHR_TAG_CLOSE);
            if (close_sqe != nullptr) {
                (void)khr_uring_submit(ring, 1);
                struct io_uring_cqe* cqe = nullptr;
                if (khr_uring_wait_cqe_timeout(ring, &cqe, 1'000) && cqe != nullptr) {
                    khr_uring_cqe_seen(ring, cqe);
                }
            }
            return false;
        }

        client->sock_fd = (int)KHR_DIRECT_SLOT_WAYLAND;
        client->is_direct = true;
        return true;
    }

    /* Pure io_uring fallback with normal file descriptor if sparse table unavailable */
    struct io_uring_sqe* sock_sqe = khr_uring_prep_socket(
        ring, AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, 0, false, KHR_TAG_WL_SOCKET);
    if (sock_sqe == nullptr) {
        return false;
    }
    if (khr_uring_submit(ring, 1) < 0) {
        return false;
    }
    struct io_uring_cqe* cqe = nullptr;
    if (!khr_uring_wait_cqe_timeout(ring, &cqe, 2'000) || cqe == nullptr) {
        return false;
    }
    int fd = cqe->res;
    khr_uring_cqe_seen(ring, cqe);
    if (fd < 0) {
        return false;
    }

    struct io_uring_sqe* conn_sqe = khr_uring_prep_connect(
        ring, fd, (struct sockaddr*)&client->sun, sizeof(client->sun), false, KHR_TAG_WL_CONNECT);
    if (conn_sqe == nullptr) {
        struct io_uring_sqe* close_sqe = khr_uring_prep_close(ring, fd, false, KHR_TAG_CLOSE);
        if (close_sqe != nullptr) {
            (void)khr_uring_submit(ring, 1);
            if (khr_uring_wait_cqe_timeout(ring, &cqe, 1'000) && cqe != nullptr) {
                khr_uring_cqe_seen(ring, cqe);
            }
        }
        return false;
    }
    if (khr_uring_submit(ring, 1) < 0) {
        return false;
    }
    if (!khr_uring_wait_cqe_timeout(ring, &cqe, 2'000) || cqe == nullptr) {
        return false;
    }
    int conn_res = cqe->res;
    khr_uring_cqe_seen(ring, cqe);
    if (conn_res < 0) {
        struct io_uring_sqe* close_sqe = khr_uring_prep_close(ring, fd, false, KHR_TAG_CLOSE);
        if (close_sqe != nullptr) {
            (void)khr_uring_submit(ring, 1);
            if (khr_uring_wait_cqe_timeout(ring, &cqe, 1'000) && cqe != nullptr) {
                khr_uring_cqe_seen(ring, cqe);
            }
        }
        return false;
    }

    client->sock_fd = fd;
    client->is_direct = false;
    return true;
}

void khr_wl_client_disconnect(khr_wl_client_t* client) {
    if (client == nullptr) {
        return;
    }
    if (client->ring != nullptr && client->sock_fd >= 0) {
        struct io_uring_sqe* sqe = khr_uring_prep_close(
            client->ring, client->sock_fd, client->is_direct, KHR_TAG_CLOSE);
        if (sqe != nullptr) {
            (void)khr_uring_submit(client->ring, 1);
            struct io_uring_cqe* cqe = nullptr;
            if (khr_uring_wait_cqe_timeout(client->ring, &cqe, 1'000) && cqe != nullptr) {
                khr_uring_cqe_seen(client->ring, cqe);
            }
        }
    }
    client->sock_fd = -1;
    client->is_direct = false;
}

[[nodiscard]]
uint32_t khr_wl_client_alloc_id(khr_wl_client_t* client) {
    if (client == nullptr) {
        return 0;
    }
    if (client->next_id <= KHR_WL_CALLBACK_ID) {
        client->next_id = KHR_WL_CALLBACK_ID + 1U;
    }
    return client->next_id++;
}

[[nodiscard]]
const khr_wl_global_t* khr_wl_client_find_global(const khr_wl_client_t* client, const char* interface) {
    if (client == nullptr || interface == nullptr) {
        return nullptr;
    }
    for (uint32_t i = 0; i < client->globals_count; i++) {
        if (strcmp(client->globals[i].interface, interface) == 0) {
            return &client->globals[i];
        }
    }
    return nullptr;
}

static void khr_wl_process_message(khr_wl_client_t* client,
                                  const khr_wl_msg_header_t* hdr,
                                  const uint8_t* payload,
                                  size_t payload_len) {
    if (hdr->object_id == KHR_WL_REGISTRY_ID && hdr->opcode == KHR_WL_REGISTRY_EVENT_GLOBAL) {
        size_t offset = 0;
        uint32_t name = 0;
        const char* iface = nullptr;
        uint32_t iface_len = 0;
        uint32_t version = 0;

        if (khr_wl_decode_u32(payload, payload_len, &offset, &name) &&
            khr_wl_decode_string(payload, payload_len, &offset, &iface, &iface_len) &&
            khr_wl_decode_u32(payload, payload_len, &offset, &version)) {

            if (client->globals_count < KHR_WL_MAX_GLOBALS) {
                auto g = &client->globals[client->globals_count++];
                g->name = name;
                g->version = version;
                strncpy(g->interface, iface, sizeof(g->interface) - 1);
                g->interface[sizeof(g->interface) - 1] = '\0';

                /* Cache core protocol shortcuts */
                if (strcmp(iface, "wl_compositor") == 0) {
                    client->compositor_name = name;
                    client->compositor_version = version;
                } else if (strcmp(iface, "xdg_wm_base") == 0) {
                    client->xdg_wm_base_name = name;
                    client->xdg_wm_base_version = version;
                } else if (strcmp(iface, "wl_shm") == 0) {
                    client->shm_name = name;
                    client->shm_version = version;
                } else if (strcmp(iface, "wl_seat") == 0) {
                    client->seat_name = name;
                    client->seat_version = version;
                } else if (strcmp(iface, "wp_linux_drm_syncobj_manager_v1") == 0) {
                    client->syncobj_manager_name = name;
                    client->syncobj_manager_version = version;
                } else if (strcmp(iface, "zwp_linux_dmabuf_v1") == 0) {
                    client->dmabuf_name = name;
                    client->dmabuf_version = version;
                }
            }
        }
    } else if (hdr->object_id == KHR_WL_CALLBACK_ID && hdr->opcode == KHR_WL_CALLBACK_EVENT_DONE) {
        client->sync_completed = true;
    } else if (hdr->object_id == KHR_WL_DISPLAY_ID &&
               hdr->opcode == KHR_WL_DISPLAY_EVENT_ERROR) {
        size_t offset = 0;
        uint32_t obj = 0;
        uint32_t code = 0;
        const char* msg = nullptr;
        uint32_t msg_len = 0;
        if (khr_wl_decode_u32(payload, payload_len, &offset, &obj) &&
            khr_wl_decode_u32(payload, payload_len, &offset, &code)) {
            client->display_error = true;
            client->error_object = obj;
            client->error_code = code;
            if (khr_wl_decode_string(payload, payload_len, &offset, &msg, &msg_len) &&
                msg != nullptr) {
                size_t n = msg_len < sizeof(client->error_msg) ? msg_len
                                                               : sizeof(client->error_msg) - 1U;
                memcpy(client->error_msg, msg, n);
                client->error_msg[n] = '\0';
            }
        }
    }
}

/* Shared wire-stream parser: consumes every complete message in [data, len).
 * Used by the bootstrap roundtrip (stack buffer) and by the non-blocking
 * PBUF consumer (provided buffers). Returns messages dispatched. */
static uint32_t khr_wl_client_consume_bytes(khr_wl_client_t* client,
                                            const uint8_t* data, size_t len,
                                            size_t* consumed) {
    uint32_t count = 0;
    size_t offset = 0;
    while (offset + 8 <= len) {
        khr_wl_msg_header_t hdr = {};
        if (!khr_wl_decode_header(data + offset, len - offset, &hdr)) {
            break;
        }
        if (hdr.size < 8 || offset + hdr.size > len) {
            break;
        }
        khr_wl_process_message(client, &hdr, data + offset + 8, hdr.size - 8);
        offset += hdr.size;
        count++;
    }
    if (consumed != nullptr) {
        *consumed = offset;
    }
    return count;
}

[[nodiscard]]
bool khr_wl_client_roundtrip(khr_wl_client_t* client) {
    if (client == nullptr || client->sock_fd < 0 || client->ring == nullptr) {
        return false;
    }

    client->sync_completed = false;

    /* Build outbound requests: get_registry + sync */
    khr_wl_msg_buf_t out = {};
    khr_wl_buf_init(&out);

    /* Request 1: wl_display.get_registry(new_id = KHR_WL_REGISTRY_ID) */
    if (!khr_wl_encode_header(&out, KHR_WL_DISPLAY_ID, KHR_WL_DISPLAY_GET_REGISTRY, 12) ||
        !khr_wl_encode_u32(&out, KHR_WL_REGISTRY_ID)) {
        return false;
    }

    /* Request 2: wl_display.sync(new_id = KHR_WL_CALLBACK_ID) */
    if (!khr_wl_encode_header(&out, KHR_WL_DISPLAY_ID, KHR_WL_DISPLAY_SYNC, 12) ||
        !khr_wl_encode_u32(&out, KHR_WL_CALLBACK_ID)) {
        return false;
    }

    /* Submit SEND SQE via raw io_uring */
    struct io_uring_sqe* send_sqe = khr_uring_prep_send(
        client->ring, client->sock_fd, out.data, out.size, 0, client->is_direct, KHR_TAG_WL_SEND);
    if (send_sqe == nullptr) {
        return false;
    }

    if (khr_uring_submit(client->ring, 1) < 0) {
        return false;
    }

    /* Wait for send completion */
    struct io_uring_cqe* cqe = nullptr;
    if (!khr_uring_wait_cqe_timeout(client->ring, &cqe, 2'000) || cqe == nullptr) {
        return false;
    }
    if (cqe->res < 0) {
        khr_uring_cqe_seen(client->ring, cqe);
        return false;
    }
    khr_uring_cqe_seen(client->ring, cqe);

    /* Bootstrap-only blocking receive: plain RECV into the stack buffer until
     * callback.done arrives. Registry discovery runs once at connect time, so
     * a bounded synchronous roundtrip is correct here; steady-state traffic
     * uses the multishot PBUF ring plus khr_wl_client_process_cqe() and never
     * blocks. */
    while (!client->sync_completed) {
        struct io_uring_sqe* recv_sqe = khr_uring_prep_recv(
            client->ring, client->sock_fd, client->in_buf, sizeof(client->in_buf), 0,
            client->is_direct, KHR_TAG_WL_RECV);
        if (recv_sqe == nullptr) {
            return false;
        }

        if (khr_uring_submit(client->ring, 1) < 0) {
            return false;
        }

        if (!khr_uring_wait_cqe_timeout(client->ring, &cqe, 2'000) || cqe == nullptr) {
            return false;
        }

        int bytes_read = cqe->res;
        khr_uring_cqe_seen(client->ring, cqe);

        if (bytes_read <= 0) {
            return false; /* Connection closed or error */
        }

        size_t used = 0;
        khr_wl_client_consume_bytes(client, client->in_buf, (size_t)bytes_read, &used);
        (void)used;
    }

    return true;
}

void khr_wl_client_attach_pbufs(khr_wl_client_t* client, khr_pbuf_t* tier0, khr_pbuf_t* tier1) {
    if (client == nullptr) {
        return;
    }
    client->pbuf_tier0 = tier0;
    client->pbuf_tier1 = tier1;
    client->recv_hdr = (struct msghdr){};
}

void khr_wl_client_attach_topology(khr_wl_client_t* client, khr_topology_t* topo) {
    if (client == nullptr) {
        return;
    }
    client->topo = topo;
}

/* Tag-matched send confirmation. With an attached topology the caller's
 * buffer stays valid until OUR send completes: await KHR_TAG_WL_SEND and
 * require the exact byte count. A short send on our blocking socket is a
 * hard failure (never a silent partial: Wayland request streams must stay
 * exact). Foreign completions observed during the wait are staged into the
 * topology queues, never swallowed. Without a topology (private bare-ring
 * tests) fall back to consuming the next CQE, sound only when nothing else
 * is in flight on that ring. */
static bool khr_wl_send_await(khr_wl_client_t* client, size_t len) {
    if (client->topo != nullptr) {
        int32_t send_res = -1;
        if (!khr_topology_await_tag(client->topo, KHR_TAG_WL_SEND, &send_res, 1'000)) {
            return false;
        }
        return send_res >= 0 && (size_t)send_res == len;
    }
    struct io_uring_cqe* cqe = nullptr;
    if (!khr_uring_wait_cqe_timeout(client->ring, &cqe, 1'000)) {
        return false;
    }
    int res = cqe->res;
    khr_uring_cqe_seen(client->ring, cqe);
    return res >= 0 && (size_t)res == len;
}

[[nodiscard]]
bool khr_wl_client_arm_inbound(khr_wl_client_t* client) {
    if (client == nullptr || client->sock_fd < 0 || client->ring == nullptr ||
        client->pbuf_tier0 == nullptr) {
        return false;
    }
    struct io_uring_sqe* sqe = khr_uring_prep_recvmsg(client->ring, client->sock_fd,
                                                      &client->recv_hdr,
                                                      client->pbuf_tier0->bgid,
                                                      true, KHR_TAG_WL_RECV);
    if (sqe == nullptr) {
        return false;
    }
    if (client->is_direct) {
        sqe->flags |= IOSQE_FIXED_FILE;
    }
    if (khr_uring_submit(client->ring, 0) < 0) {
        return false;
    }
    client->inbound_armed = true;
    return true;
}

[[nodiscard]]
bool khr_wl_client_arm_inbound_tier1(khr_wl_client_t* client) {
    if (client == nullptr || client->sock_fd < 0 || client->ring == nullptr ||
        client->pbuf_tier1 == nullptr) {
        return false;
    }
    struct io_uring_sqe* sqe = khr_uring_prep_recvmsg(client->ring, client->sock_fd,
                                                      &client->recv_hdr,
                                                      client->pbuf_tier1->bgid,
                                                      true, KHR_TAG_WL_RECV_T1);
    if (sqe == nullptr) {
        return false;
    }
    if (client->is_direct) {
        sqe->flags |= IOSQE_FIXED_FILE;
    }
    if (khr_uring_submit(client->ring, 0) < 0) {
        return false;
    }
    client->inbound_armed = true;
    return true;
}

[[nodiscard]]
bool khr_wl_client_send_skip(khr_wl_client_t* client, const void* data, size_t len) {
    if (client == nullptr || client->sock_fd < 0 || client->ring == nullptr ||
        data == nullptr || len == 0) {
        return false;
    }
    client->send_iov = (struct iovec){
        .iov_base = (void*)data,
        .iov_len = len,
    };
    client->send_hdr = (struct msghdr){
        .msg_iov = &client->send_iov,
        .msg_iovlen = 1,
    };
    /* Report completion (no SKIP_SUCCESS): the byte count in the SEND CQE
     * is the only proof the exact request stream hit the socket. One CQE
     * per send, no linked barrier, zero completion debt by construction. */
    struct io_uring_sqe* sqe = khr_uring_prep_sendmsg(client->ring, client->sock_fd,
                                                      &client->send_hdr, false,
                                                      KHR_TAG_WL_SEND);
    if (sqe == nullptr) {
        return false;
    }
    if (client->is_direct) {
        sqe->flags |= IOSQE_FIXED_FILE;
    }
    return khr_wl_send_await(client, len);
}

[[nodiscard]]
bool khr_wl_client_send_with_fd(khr_wl_client_t* client, const void* data,
                                size_t len, int fd) {
    if (client == nullptr || client->sock_fd < 0 || client->ring == nullptr ||
        data == nullptr || len == 0 || fd < 0) {
        return false;
    }
    client->send_iov = (struct iovec){
        .iov_base = (void*)data,
        .iov_len = len,
    };
    /* Ancillary payload: exactly one FD. CMSG_SPACE covers alignment. */
    static_assert(CMSG_SPACE(sizeof(int)) <= sizeof(client->send_cmsg),
                  "send cmsg scratch too small for one fd");
    client->send_hdr = (struct msghdr){
        .msg_iov = &client->send_iov,
        .msg_iovlen = 1,
        .msg_control = client->send_cmsg,
        .msg_controllen = CMSG_SPACE(sizeof(int)),
    };
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&client->send_hdr);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));

    /* Same contract as send_skip: exactly one completion carrying the byte
     * count, awaited by tag before the caller reuses its buffer. */
    struct io_uring_sqe* sqe = khr_uring_prep_sendmsg(client->ring, client->sock_fd,
                                                      &client->send_hdr, false,
                                                      KHR_TAG_WL_SEND);
    if (sqe == nullptr) {
        return false;
    }
    if (client->is_direct) {
        sqe->flags |= IOSQE_FIXED_FILE;
    }
    return khr_wl_send_await(client, len);
}

uint32_t khr_wl_client_feed_cqe(khr_wl_client_t* client, uint64_t user_data,
                                int32_t res, uint32_t flags,
                                const uint8_t** out_data, size_t* out_len,
                                bool* need_rearm) {
    if (out_data != nullptr) {
        *out_data = nullptr;
    }
    if (out_len != nullptr) {
        *out_len = 0;
    }
    if (need_rearm != nullptr) {
        *need_rearm = false;
    }
    if (client == nullptr) {
        return 0;
    }
    if (res == -ENOBUFS &&
        (user_data == KHR_TAG_WL_RECV || user_data == KHR_TAG_WL_RECV_T1)) {
        client->inbound_armed = false;
        if (need_rearm != nullptr) {
            *need_rearm = true;
        }
        return 0;
    }
    if (res <= 0) {
        return 0;
    }
    if (client->in_off > 0 && client->in_off <= client->in_len) {
        size_t left = client->in_len - client->in_off;
        if (left > 0) {
            memmove(client->in_buf, client->in_buf + client->in_off, left);
        }
        client->in_len = left;
        client->in_off = 0;
    }
    khr_pbuf_t* tier = nullptr;
    if (user_data == KHR_TAG_WL_RECV) {
        tier = client->pbuf_tier0;
    } else if (user_data == KHR_TAG_WL_RECV_T1) {
        tier = client->pbuf_tier1;
    } else {
        return 0;
    }
    if (tier == nullptr) {
        return 0;
    }
    uint16_t bid = (uint16_t)(flags >> IORING_CQE_BUFFER_SHIFT);
    uint8_t* raw = khr_pbuf_data(tier, bid);
    if (raw == nullptr) {
        return 0;
    }
    khr_recvmsg_view_t view = {};
    uint32_t namelen = (uint32_t)client->recv_hdr.msg_namelen;
    uint32_t controllen = (uint32_t)client->recv_hdr.msg_controllen;
    if (!khr_recvmsg_parse(raw, tier->buf_size, namelen, controllen, &view) ||
        view.payload == nullptr || view.payload_len == 0) {
        return 0;
    }
    if (view.control != nullptr && controllen > 0) {
        struct msghdr fake = {
            .msg_control = (void*)view.control,
            .msg_controllen = controllen,
        };
        for (struct cmsghdr* c = CMSG_FIRSTHDR(&fake); c != nullptr;
             c = CMSG_NXTHDR(&fake, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS &&
                c->cmsg_len >= CMSG_LEN(sizeof(int))) {
                int fd = -1;
                memcpy(&fd, CMSG_DATA(c), sizeof(fd));
                if (fd >= 0) {
                    close(fd);
                }
            }
        }
    }
    if (client->in_len + view.payload_len > sizeof(client->in_buf)) {
        client->in_len = 0;
    }
    if (client->in_len + view.payload_len > sizeof(client->in_buf)) {
        return 0;
    }
    memcpy(client->in_buf + client->in_len, view.payload, view.payload_len);
    client->in_len += view.payload_len;
    size_t used = 0;
    uint32_t count = khr_wl_client_consume_bytes(client, client->in_buf,
                                                 client->in_len, &used);
    if (out_data != nullptr) {
        *out_data = client->in_buf;
    }
    if (out_len != nullptr) {
        *out_len = used;
    }
    client->in_off = used;
    return count;
}

uint32_t khr_wl_client_process_cqe(khr_wl_client_t* client, uint64_t user_data,
                                   int32_t res, uint32_t flags) {
    bool rearm = false;
    return khr_wl_client_feed_cqe(client, user_data, res, flags,
                                  nullptr, nullptr, &rearm);
}

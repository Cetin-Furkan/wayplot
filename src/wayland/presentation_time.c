#include "khoros/wayland/presentation_time.h"

#if __STDC_VERSION__ < 202311L
#error "Khoros requires pure ISO C23 (compile with -std=c23)"
#endif

#ifdef _STDBOOL_H
#error "<stdbool.h> is strictly banned. In C23, bool, true, and false are native language keywords."
#endif

#ifdef _STDALIGN_H
#error "<stdalign.h> is strictly banned. In C23, alignas and alignof are native language keywords."
#endif

#include <string.h>
#include <time.h>

[[nodiscard]]
bool khr_presentation_time_bind(khr_presentation_time_t* pt, khr_wl_client_t* client) {
    if (pt == nullptr || client == nullptr) {
        return false;
    }
    *pt = (khr_presentation_time_t){
        .client = client,
        .clock_id = 1, /* CLOCK_MONOTONIC default */
    };

    const khr_wl_global_t* g = khr_wl_client_find_global(client, KHR_WP_PRESENTATION_INTERFACE);
    if (g == nullptr) {
        return false;
    }

    uint32_t id = khr_wl_client_alloc_id(client);
    if (id == 0) {
        return false;
    }

    khr_wl_msg_buf_t out = {};
    khr_wl_buf_init(&out);
    const char* iface = KHR_WP_PRESENTATION_INTERFACE;
    uint32_t slen = (uint32_t)strlen(iface) + 1U;
    uint16_t total = (uint16_t)(24U + khr_wl_pad4(slen));

    uint32_t version = (g->version > KHR_WP_PRESENTATION_VERSION) ? KHR_WP_PRESENTATION_VERSION : g->version;
    if (!khr_wl_encode_header(&out, KHR_WL_REGISTRY_ID, KHR_WL_REGISTRY_BIND, total) ||
        !khr_wl_encode_u32(&out, g->name) ||
        !khr_wl_encode_string(&out, iface) ||
        !khr_wl_encode_u32(&out, version) ||
        !khr_wl_encode_u32(&out, id) ||
        !khr_wl_client_send_skip(client, out.data, out.size)) {
        return false;
    }

    pt->wp_presentation_id = id;
    pt->bound = true;
    return true;
}

void khr_presentation_time_destroy(khr_presentation_time_t* pt) {
    if (pt == nullptr) {
        return;
    }
    if (pt->bound && pt->client != nullptr && pt->wp_presentation_id != 0) {
        khr_wl_msg_buf_t out = {};
        khr_wl_buf_init(&out);
        if (khr_wl_encode_header(&out, pt->wp_presentation_id, KHR_WP_PRESENTATION_DESTROY, 8)) {
            (void)khr_wl_client_send_skip(pt->client, out.data, out.size);
        }
    }
    *pt = (khr_presentation_time_t){};
}

[[nodiscard]]
bool khr_presentation_request_feedback(khr_presentation_time_t* pt,
                                       uint32_t surface_id,
                                       uint64_t commit_seq,
                                       uint32_t* out_feedback_id) {
    if (pt == nullptr || !pt->bound || pt->client == nullptr ||
        pt->wp_presentation_id == 0 || surface_id == 0) {
        return false;
    }

    uint32_t fid = khr_wl_client_alloc_id(pt->client);
    if (fid == 0) {
        return false;
    }

    khr_wl_msg_buf_t out = {};
    khr_wl_buf_init(&out);
    if (!khr_wl_encode_header(&out, pt->wp_presentation_id, KHR_WP_PRESENTATION_FEEDBACK, 16) ||
        !khr_wl_encode_u32(&out, surface_id) ||
        !khr_wl_encode_u32(&out, fid) ||
        !khr_wl_client_send_skip(pt->client, out.data, out.size)) {
        return false;
    }

    /* Record request in pending tracking table */
    struct timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now_ns = (uint64_t)ts.tv_sec * 1'000'000'000ULL + (uint64_t)ts.tv_nsec;

    int slot_idx = -1;
    for (size_t i = 0; i < KHR_PRESENTATION_MAX_PENDING; i++) {
        if (!pt->pending[i].pending) {
            slot_idx = (int)i;
            break;
        }
    }
    if (slot_idx < 0) {
        /* Table full: evict oldest entry at index 0 */
        for (size_t i = 0; i < KHR_PRESENTATION_MAX_PENDING - 1; i++) {
            pt->pending[i] = pt->pending[i + 1];
        }
        slot_idx = (int)(KHR_PRESENTATION_MAX_PENDING - 1);
    } else {
        pt->pending_count++;
    }

    pt->pending[slot_idx] = (khr_presentation_request_t){
        .feedback_id = fid,
        .commit_seq = commit_seq,
        .commit_time_ns = now_ns,
        .pending = true,
    };

    if (out_feedback_id != nullptr) {
        *out_feedback_id = fid;
    }
    return true;
}

uint32_t khr_presentation_time_consume(khr_presentation_time_t* pt,
                                       const uint8_t* data,
                                       size_t len) {
    if (pt == nullptr || data == nullptr || len < 8) {
        return 0;
    }

    uint32_t processed_count = 0;
    size_t offset = 0;

    while (offset + 8 <= len) {
        khr_wl_msg_header_t hdr = {};
        if (!khr_wl_decode_header(data + offset, len - offset, &hdr)) {
            break;
        }
        if (hdr.size < 8 || offset + hdr.size > len) {
            break;
        }

        const uint8_t* body = data + offset + 8;
        size_t body_len = (size_t)hdr.size - 8U;

        if (hdr.object_id == pt->wp_presentation_id) {
            if (hdr.opcode == KHR_WP_PRESENTATION_EVENT_CLOCK_ID && body_len >= 4) {
                uint32_t clk = 0;
                size_t p_off = 0;
                if (khr_wl_decode_u32(body, body_len, &p_off, &clk)) {
                    pt->clock_id = clk;
                }
            }
            offset += hdr.size;
            continue;
        }

        /* Check if message targets an in-flight feedback request */
        int found_slot = -1;
        for (size_t i = 0; i < KHR_PRESENTATION_MAX_PENDING; i++) {
            if (pt->pending[i].pending && pt->pending[i].feedback_id == hdr.object_id) {
                found_slot = (int)i;
                break;
            }
        }

        if (found_slot >= 0) {
            khr_presentation_request_t* req = &pt->pending[found_slot];
            if (hdr.opcode == KHR_WP_PRESENTATION_FEEDBACK_EVENT_PRESENTED && body_len >= 28) {
                uint32_t sec_hi = 0, sec_lo = 0, nsec = 0, refresh = 0, seq_hi = 0, seq_lo = 0, flags = 0;
                size_t p_off = 0;
                if (khr_wl_decode_u32(body, body_len, &p_off, &sec_hi) &&
                    khr_wl_decode_u32(body, body_len, &p_off, &sec_lo) &&
                    khr_wl_decode_u32(body, body_len, &p_off, &nsec) &&
                    khr_wl_decode_u32(body, body_len, &p_off, &refresh) &&
                    khr_wl_decode_u32(body, body_len, &p_off, &seq_hi) &&
                    khr_wl_decode_u32(body, body_len, &p_off, &seq_lo) &&
                    khr_wl_decode_u32(body, body_len, &p_off, &flags)) {

                    uint64_t sec = ((uint64_t)sec_hi << 32) | (uint64_t)sec_lo;
                    uint64_t pres_time_ns = sec * 1'000'000'000ULL + (uint64_t)nsec;
                    uint64_t seq = ((uint64_t)seq_hi << 32) | (uint64_t)seq_lo;
                    uint64_t latency_ns = (pres_time_ns >= req->commit_time_ns)
                                              ? (pres_time_ns - req->commit_time_ns)
                                              : 0;
                    int64_t jitter_ns = 0;
                    if (pt->last_presentation_time_ns > 0 && pres_time_ns > pt->last_presentation_time_ns) {
                        int64_t dt = (int64_t)(pres_time_ns - pt->last_presentation_time_ns);
                        jitter_ns = dt - (int64_t)refresh;
                    }

                    pt->last_presentation_time_ns = pres_time_ns;
                    pt->last_sequence = seq;
                    pt->refresh_ns = refresh;
                    pt->flags = flags;
                    pt->last_latency_ns = latency_ns;
                    pt->last_jitter_ns = jitter_ns;
                    pt->total_presented++;

                    pt->last_sample = (khr_presentation_feedback_sample_t){
                        .presentation_time_ns = pres_time_ns,
                        .sequence = seq,
                        .refresh_ns = refresh,
                        .flags = flags,
                        .commit_seq = req->commit_seq,
                        .latency_ns = latency_ns,
                        .jitter_ns = jitter_ns,
                        .presented = true,
                    };
                    pt->has_sample = true;

                    req->pending = false;
                    if (pt->pending_count > 0) pt->pending_count--;
                    processed_count++;
                }
            } else if (hdr.opcode == KHR_WP_PRESENTATION_FEEDBACK_EVENT_DISCARDED) {
                pt->total_discarded++;
                pt->last_sample = (khr_presentation_feedback_sample_t){
                    .commit_seq = req->commit_seq,
                    .presented = false,
                };
                pt->has_sample = true;

                req->pending = false;
                if (pt->pending_count > 0) pt->pending_count--;
                processed_count++;
            } else if (hdr.opcode == KHR_WP_PRESENTATION_FEEDBACK_EVENT_SYNC_OUTPUT) {
                /* Optional display output binding information */
            }
        }

        offset += hdr.size;
    }

    return processed_count;
}

[[nodiscard]]
bool khr_presentation_predict_next_vsync(const khr_presentation_time_t* pt,
                                         uint64_t now_ns,
                                         uint64_t* out_next_vsync_ns,
                                         uint32_t* out_refresh_ns) {
    if (pt == nullptr || !pt->bound || pt->refresh_ns == 0 || pt->last_presentation_time_ns == 0) {
        return false;
    }

    uint64_t refresh = (uint64_t)pt->refresh_ns;
    uint64_t last = pt->last_presentation_time_ns;
    uint64_t next_vsync = last;

    if (now_ns >= last) {
        uint64_t elapsed = now_ns - last;
        uint64_t periods = (elapsed / refresh) + 1ULL;
        next_vsync = last + (periods * refresh);
    } else {
        next_vsync = last + refresh;
    }

    if (out_next_vsync_ns != nullptr) {
        *out_next_vsync_ns = next_vsync;
    }
    if (out_refresh_ns != nullptr) {
        *out_refresh_ns = pt->refresh_ns;
    }
    return true;
}

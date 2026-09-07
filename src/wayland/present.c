#include "khoros/wayland/present.h"

#include <string.h>

[[nodiscard]]
bool khr_present_init(khr_wl_client_t* client, uint32_t surface_id,
                      uint32_t w, uint32_t h, khr_present_t* out) {
    if (client == nullptr || surface_id == 0 || w == 0 || h == 0 ||
        w > 4'096 || h > 4'096 || out == nullptr) {
        return false;
    }
    *out = (khr_present_t){};
    out->client = client;
    out->surface_id = surface_id;
    out->width = w;
    out->height = h;
    out->stride = w * 4U;

    if (!khr_shm_bind(client, &out->shm_id)) {
        return false;
    }
    size_t pool_size = (size_t)out->stride * h * KHR_PRESENT_SLOTS;
    if (!khr_shm_pool_init(client, out->shm_id, pool_size, &out->pool)) {
        return false;
    }
    for (uint32_t i = 0; i < KHR_PRESENT_SLOTS; i++) {
        uint32_t offset = i * out->stride * h;
        uint32_t buffer_id = 0;
        if (!khr_shm_buffer_create(client, &out->pool, offset, w, h,
                                   out->stride, &buffer_id)) {
            khr_present_destroy(out);
            return false;
        }
        out->slots[i] = (khr_present_slot_t){
            .buffer_id = buffer_id,
            .offset = offset,
            .busy = false,
        };
    }
    return true;
}

[[nodiscard]]
uint8_t* khr_present_slot_pixels(khr_present_t* p, uint32_t slot) {
    if (p == nullptr || p->pool.addr == nullptr || slot >= KHR_PRESENT_SLOTS) {
        return nullptr;
    }
    return (uint8_t*)p->pool.addr + p->slots[slot].offset;
}

void khr_present_paint_test(khr_present_t* p, uint8_t* pixels, uint64_t frame_no) {
    if (p == nullptr || pixels == nullptr) {
        return;
    }
    uint8_t b = (uint8_t)(frame_no & 0xFFU);
    uint32_t xden = (p->width > 1U) ? p->width - 1U : 1U;
    uint32_t yden = (p->height > 1U) ? p->height - 1U : 1U;
    for (uint32_t y = 0; y < p->height; y++) {
        for (uint32_t x = 0; x < p->width; x++) {
            uint8_t* px = pixels + ((size_t)y * p->width + x) * 4;
            px[0] = b;                                        /* B */
            px[1] = (uint8_t)((uint64_t)y * 255U / yden);     /* G */
            px[2] = (uint8_t)((uint64_t)x * 255U / xden);     /* R */
            px[3] = 255;                                      /* A */
        }
    }
}

[[nodiscard]]
uint32_t khr_present_next_free(const khr_present_t* p) {
    if (p == nullptr) {
        return UINT32_MAX;
    }
    for (uint32_t i = 0; i < KHR_PRESENT_SLOTS; i++) {
        uint32_t idx = (p->next_slot + i) % KHR_PRESENT_SLOTS;
        if (!p->slots[idx].busy) {
            return idx;
        }
    }
    return UINT32_MAX;
}

[[nodiscard]]
bool khr_present_commit(khr_present_t* p, uint32_t slot) {
    if (p == nullptr || p->client == nullptr || slot >= KHR_PRESENT_SLOTS) {
        return false;
    }
    if (p->slots[slot].busy) {
        return false;
    }
    if (!khr_shm_attach_commit(p->client, p->surface_id,
                               p->slots[slot].buffer_id, p->width, p->height)) {
        return false;
    }
    p->slots[slot].busy = true;
    p->next_slot = (slot + 1U) % KHR_PRESENT_SLOTS;
    p->frames++;
    return true;
}

uint32_t khr_present_consume(khr_present_t* p, const uint8_t* data, size_t len) {
    if (p == nullptr || data == nullptr) {
        return 0;
    }
    uint32_t count = 0;
    size_t offset = 0;
    while (offset + 8 <= len) {
        khr_wl_msg_header_t hdr = {};
        if (!khr_wl_decode_header(data + offset, len - offset, &hdr)) {
            break;
        }
        if (hdr.size != 8 || offset + hdr.size > len) {
            break;
        }
        if (hdr.opcode == KHR_WL_BUFFER_EVENT_RELEASE) {
            for (uint32_t i = 0; i < KHR_PRESENT_SLOTS; i++) {
                if (p->slots[i].buffer_id == hdr.object_id && p->slots[i].busy) {
                    p->slots[i].busy = false;
                    p->releases++;
                    count++;
                    break;
                }
            }
        }
        offset += hdr.size;
    }
    return count;
}

void khr_present_destroy(khr_present_t* p) {
    if (p == nullptr) {
        return;
    }
    if (p->client != nullptr) {
        for (uint32_t i = 0; i < KHR_PRESENT_SLOTS; i++) {
            if (p->slots[i].buffer_id != 0) {
                (void)khr_shm_buffer_destroy(p->client, p->slots[i].buffer_id);
            }
        }
    }
    khr_shm_pool_destroy(p->client, &p->pool);
    *p = (khr_present_t){};
}

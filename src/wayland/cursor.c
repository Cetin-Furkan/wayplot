#include "khoros/wayland/cursor.h"
#include "khoros/wayland/seat.h"
#include "khoros/wayland/xdg.h"
#include "khoros/wayland/wire.h"

#include <string.h>
#include <unistd.h>

void khr_cursor_init(khr_cursor_t* cur) {
    if (cur != nullptr) {
        *cur = (khr_cursor_t){ .pool = { .fd = -1 } };
    }
}

[[nodiscard]]
static uint32_t khr_cursor_rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    /* wl_shm ARGB8888 word is AARRGGBB; little-endian bytes B,G,R,A. */
    return b | (g << 8) | (r << 16) | (a << 24);
}

static void khr_cursor_paint_arrow(uint32_t* px, uint32_t dim) {
    for (uint32_t i = 0; i < dim * dim; i++) {
        px[i] = 0;
    }
    const uint32_t ink = khr_cursor_rgba(255, 255, 255, 255);
    const uint32_t edge = khr_cursor_rgba(20, 20, 20, 255);
    for (uint32_t y = 1; y < 16 && y < dim; y++) {
        for (uint32_t x = 1; x <= y && x < dim; x++) {
            px[y * dim + x] = (x == 1 || x == y || y == 15) ? edge : ink;
        }
    }
}

[[nodiscard]]
bool khr_cursor_setup(khr_wl_client_t* client, uint32_t compositor_id,
                      uint32_t pointer_id, khr_cursor_t* cur) {
    if (client == nullptr || cur == nullptr) {
        return false;
    }
    khr_cursor_init(cur);
    const khr_wl_global_t* shape =
        khr_wl_client_find_global(client, "wp_cursor_shape_manager_v1");
    if (shape != nullptr && pointer_id != 0) {
        cur->mgr_id = khr_wl_client_alloc_id(client);
        cur->device_id = khr_wl_client_alloc_id(client);
        if (cur->mgr_id == 0 || cur->device_id == 0) {
            return false;
        }
        uint32_t slen = (uint32_t)strlen("wp_cursor_shape_manager_v1") + 1U;
        uint16_t bind_sz = (uint16_t)(24U + khr_wl_pad4(slen));
        khr_wl_msg_buf_t out = {};
        khr_wl_buf_init(&out);
        if (!khr_wl_encode_header(&out, KHR_WL_REGISTRY_ID, KHR_WL_REGISTRY_BIND, bind_sz) ||
            !khr_wl_encode_u32(&out, shape->name) ||
            !khr_wl_encode_string(&out, "wp_cursor_shape_manager_v1") ||
            !khr_wl_encode_u32(&out, shape->version) ||
            !khr_wl_encode_u32(&out, cur->mgr_id) ||
            !khr_wl_encode_header(&out, cur->mgr_id, KHR_CURSOR_SHAPE_GET_POINTER, 16) ||
            !khr_wl_encode_u32(&out, cur->device_id) ||
            !khr_wl_encode_u32(&out, pointer_id) ||
            !khr_wl_client_send_skip(client, out.data, out.size)) {
            return false;
        }
        cur->shape_proto = true;
        /* Fall through: also keep a shm arrow. Mutter often ignores
         * set_shape until set_cursor has run once. */
    }
    if (compositor_id == 0 || pointer_id == 0) {
        return cur->shape_proto;
    }
    if (!khr_shm_bind(client, &cur->shm_id)) {
        return true;
    }
    constexpr uint32_t dim = KHR_WINDOW_CURSOR_PX;
    constexpr size_t bytes = (size_t)dim * (size_t)dim * 4U;
    if (!khr_shm_pool_init(client, cur->shm_id, bytes, &cur->pool)) {
        return cur->shape_proto;
    }
    khr_cursor_paint_arrow((uint32_t*)cur->pool.addr, dim);
    cur->surface_id = khr_wl_client_alloc_id(client);
    if (cur->surface_id == 0 ||
        !khr_shm_buffer_create(client, &cur->pool, 0, dim, dim, dim * 4U,
                               &cur->buffer_id)) {
        khr_shm_pool_destroy(client, &cur->pool);
        return cur->shape_proto;
    }
    khr_wl_msg_buf_t out = {};
    khr_wl_buf_init(&out);
    if (!khr_wl_encode_header(&out, compositor_id, KHR_WL_COMPOSITOR_CREATE_SURFACE, 12) ||
        !khr_wl_encode_u32(&out, cur->surface_id) ||
        !khr_wl_client_send_skip(client, out.data, out.size) ||
        !khr_shm_attach_commit(client, cur->surface_id, cur->buffer_id, dim, dim)) {
        khr_shm_pool_destroy(client, &cur->pool);
        return cur->shape_proto;
    }
    cur->shm_live = true;
    return true;
}

[[nodiscard]]
bool khr_cursor_apply(khr_wl_client_t* client, khr_cursor_t* cur,
                      uint32_t pointer_id, uint32_t serial, khr_hit_t hit,
                      bool force) {
    if (client == nullptr || cur == nullptr || pointer_id == 0 || serial == 0) {
        return false;
    }
    uint32_t shape = khr_hit_cursor_shape(hit);
    if (!force && cur->last_serial == serial && cur->last_shape == shape) {
        return true;
    }
    if (cur->shape_proto && cur->device_id != 0) {
        khr_wl_msg_buf_t out = {};
        khr_wl_buf_init(&out);
        if (!khr_wl_encode_header(&out, cur->device_id, KHR_CURSOR_SHAPE_SET_SHAPE, 16) ||
            !khr_wl_encode_u32(&out, serial) ||
            !khr_wl_encode_u32(&out, shape) ||
            !khr_wl_client_send_skip(client, out.data, out.size)) {
            return false;
        }
    }
    if (!cur->shm_live || cur->surface_id == 0) {
        cur->last_shape = shape;
        cur->last_serial = serial;
        return true;
    }
    khr_wl_msg_buf_t out = {};
    khr_wl_buf_init(&out);
    if (!khr_wl_encode_header(&out, pointer_id, KHR_WL_POINTER_SET_CURSOR, 24) ||
        !khr_wl_encode_u32(&out, serial) ||
        !khr_wl_encode_u32(&out, cur->surface_id) ||
        !khr_wl_encode_i32(&out, 1) ||
        !khr_wl_encode_i32(&out, 1) ||
        !khr_wl_client_send_skip(client, out.data, out.size)) {
        return false;
    }
    cur->last_shape = shape;
    cur->last_serial = serial;
    return true;
}

void khr_cursor_destroy(khr_wl_client_t* client, khr_cursor_t* cur) {
    if (cur == nullptr) {
        return;
    }
    if (cur->shm_live) {
        if (cur->buffer_id != 0) {
            (void)khr_shm_buffer_destroy(client, cur->buffer_id);
        }
        khr_shm_pool_destroy(client, &cur->pool);
    }
    khr_cursor_init(cur);
}

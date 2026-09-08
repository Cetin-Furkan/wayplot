#ifndef KHOROS_GFX_BLOB_H
#define KHOROS_GFX_BLOB_H

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
#include "khoros/gfx/pipeline.h"

/*
 * Packed mesh blob. All pointers are self-relative (offset from the field).
 * Ring B DMA's the file into the hugepage payload; CPU validates once;
 * the GPU reads verts/indices through BDA. No fixup pass.
 *
 * Layout:
 *   header (32 B)
 *   packed float3 verts  [vert_count]
 *   uint32 indices       [index_count]   (triangle list, CCW)
 */
constexpr uint32_t KHR_BLOB_MAGIC   = 0x4252484BU; /* 'KHRB' LE */
constexpr uint32_t KHR_BLOB_VERSION = 1;
constexpr uint32_t KHR_BLOB_MAX_VERTS   = 262'144;
constexpr uint32_t KHR_BLOB_MAX_INDICES = 786'432;

typedef struct khr_blob {
    uint32_t magic;
    uint32_t version;
    uint32_t vert_count;
    uint32_t index_count;
    int32_t  verts_off;    /* from &verts_off */
    int32_t  indices_off;  /* from &indices_off */
    uint32_t flags;
    uint32_t _pad;
} khr_blob_t;

static_assert(sizeof(khr_blob_t) == 32, "blob header is 32 bytes");

typedef struct khr_blob_view {
    uint32_t     vert_count;
    uint32_t     index_count;
    const float* verts;     /* xyz packed, count * 3 floats */
    const uint32_t* indices;
    size_t       verts_byte_off;
    size_t       indices_byte_off;
} khr_blob_view_t;

[[nodiscard]]
static inline const void* khr_rel_ptr(const void* field, int32_t off) {
    if (field == nullptr || off == 0) {
        return nullptr;
    }
    return (const uint8_t*)field + off;
}

[[nodiscard]]
static inline size_t khr_blob_bytes(uint32_t vert_count, uint32_t index_count) {
    return sizeof(khr_blob_t) +
           (size_t)vert_count * 12U +
           (size_t)index_count * 4U;
}

[[nodiscard]]
static inline bool khr_blob_parse(const void* base, size_t cap, khr_blob_view_t* out) {
    if (base == nullptr || cap < sizeof(khr_blob_t) || out == nullptr) {
        return false;
    }
    const khr_blob_t* h = (const khr_blob_t*)base;
    if (h->magic != KHR_BLOB_MAGIC || h->version != KHR_BLOB_VERSION) {
        return false;
    }
    if (h->vert_count == 0 || h->vert_count > KHR_BLOB_MAX_VERTS ||
        h->index_count < 3U || (h->index_count % 3U) != 0U ||
        h->index_count > KHR_BLOB_MAX_INDICES) {
        return false;
    }
    const float* verts = (const float*)khr_rel_ptr(&h->verts_off, h->verts_off);
    const uint32_t* idx = (const uint32_t*)khr_rel_ptr(&h->indices_off, h->indices_off);
    if (verts == nullptr || idx == nullptr) {
        return false;
    }
    const uint8_t* b0 = (const uint8_t*)base;
    const uint8_t* b1 = b0 + cap;
    size_t vbytes = (size_t)h->vert_count * 12U;
    size_t ibytes = (size_t)h->index_count * 4U;
    if ((const uint8_t*)verts < b0 || (const uint8_t*)verts + vbytes > b1) {
        return false;
    }
    if ((const uint8_t*)idx < b0 || (const uint8_t*)idx + ibytes > b1) {
        return false;
    }
    for (uint32_t i = 0; i < h->index_count; i++) {
        if (idx[i] >= h->vert_count) {
            return false;
        }
    }
    *out = (khr_blob_view_t){
        .vert_count = h->vert_count,
        .index_count = h->index_count,
        .verts = verts,
        .indices = idx,
        .verts_byte_off = (size_t)((const uint8_t*)verts - b0),
        .indices_byte_off = (size_t)((const uint8_t*)idx - b0),
    };
    return true;
}

[[nodiscard]]
static inline bool khr_mesh_bind_blob(const void* base, size_t cap,
                                      VkDeviceAddress gpu_base,
                                      khr_mesh_push_t* push) {
    khr_blob_view_t v = {};
    if (gpu_base == 0 || push == nullptr || !khr_blob_parse(base, cap, &v)) {
        return false;
    }
    push->verts_addr = gpu_base + (VkDeviceAddress)v.verts_byte_off;
    push->indices_addr = gpu_base + (VkDeviceAddress)v.indices_byte_off;
    push->index_count = v.index_count;
    push->vert_count = v.vert_count;
    push->pad0 = 0;
    push->pad1 = 0;
    return true;
}

/* Packed verts (xyz * nv) + triangle indices. */
[[nodiscard]]
size_t khr_blob_write(void* dst, size_t cap, const float* xyz, uint32_t nv,
                      const uint32_t* idx, uint32_t ni);

/* Unit box, 8 verts / 36 indices. */
[[nodiscard]]
size_t khr_blob_write_box(void* dst, size_t cap);

typedef struct khr_cam {
    float yaw;
    float pitch;
    float radius;
    float target[3];
    float r0[3];
    float r1[3];
    float r2[3];
} khr_cam_t;

void khr_cam_frame(khr_cam_t* cam, const float* xyz, uint32_t nv, bool reset_orient);
void khr_cam_orbit(khr_cam_t* cam, float dyaw, float dpitch);
void khr_cam_pan(khr_cam_t* cam, float dx, float dy);
void khr_cam_zoom(khr_cam_t* cam, float ticks);
void khr_cam_snap_axis(khr_cam_t* cam, int axis); /* ±1 look-from X, ±2 Y, ±3 Z */
[[nodiscard]]
int khr_cam_pick_axis(const khr_cam_t* cam, float nx, float ny);
void khr_mesh_cam_apply(khr_mesh_push_t* push, const khr_cam_t* cam);
void khr_gizmo_arm_apply(khr_mesh_push_t* push, const float* r0, const float* r1,
                         const float* r2, int axis, uint32_t rgb);

/* Center/scale + yaw/pitch so the AABB fills the client. CPU once. */
void khr_mesh_fit_view(khr_mesh_push_t* push, const float* xyz, uint32_t nv);

/* Parse, bind BDA, fit camera. */
[[nodiscard]]
bool khr_mesh_setup(const void* base, size_t cap, VkDeviceAddress gpu_base,
                    khr_mesh_push_t* push);

[[nodiscard]]
size_t khr_blob_write_gizmo_arm(void* dst, size_t cap);

#endif /* KHOROS_GFX_BLOB_H */

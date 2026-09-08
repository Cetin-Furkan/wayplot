#include "khoros/wayland/dmabuf_present.h"
#include "khoros/wayland/shm.h"

#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <drm/drm.h>
#include <vulkan/vulkan.h>

/* DRM timeline helpers on the device render node. All proven green on the
 * Xe experimental stack (native syncobj ioctls, no Vulkan interop). */

#ifndef DRM_SYNCOBJ_CREATE_TIMELINE
#define DRM_SYNCOBJ_CREATE_TIMELINE (1U << 1)
#endif

[[nodiscard]]
static uint32_t khr_dmp_create_timeline(int drm_fd) {
    if (drm_fd < 0) {
        return 0;
    }
    struct drm_syncobj_create create = {
        .flags = DRM_SYNCOBJ_CREATE_TIMELINE,
    };
    if (ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_CREATE, &create) != 0 ||
        create.handle == 0) {
        /* Older UAPI: this kernel still accepts TIMELINE_SIGNAL on a
         * flags=0 syncobj (proven on Xe). Do not fail the loop. */
        create = (struct drm_syncobj_create){};
        if (ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_CREATE, &create) != 0 ||
            create.handle == 0) {
            return 0;
        }
    }
    return create.handle;
}

static void khr_dmp_destroy_timeline(int drm_fd, uint32_t handle) {
    if (drm_fd < 0 || handle == 0) {
        return;
    }
    struct drm_syncobj_destroy destroy = {};
    destroy.handle = handle;
    (void)ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
}

/* Export a DRM syncobj as an importable fd (plain syncobj fd, no flags:
 * import_timeline takes syncobj fds, not sync_files). */
[[nodiscard]]
static bool khr_dmp_export_fd(int drm_fd, uint32_t handle, int* out_fd) {
    struct drm_syncobj_handle h2f = {};
    h2f.handle = handle;
    if (ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &h2f) != 0 ||
        h2f.fd < 0) {
        return false;
    }
    *out_fd = h2f.fd;
    return true;
}

[[nodiscard]]
static bool khr_dmp_signal(int drm_fd, uint32_t handle, uint64_t point) {
    if (drm_fd < 0 || handle == 0 || point == 0) {
        return false;
    }
    uint64_t hs[1] = { handle };
    uint64_t ps[1] = { point };
    struct drm_syncobj_timeline_array arr = {};
    arr.handles = (uint64_t)(uintptr_t)hs;
    arr.points = (uint64_t)(uintptr_t)ps;
    arr.count_handles = 1;
    return ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &arr) == 0;
}

static void khr_dmp_retire_slot(khr_gfx_device_t* dev, khr_wl_client_t* client,
                                khr_dmabuf_pslot_t* slot) {
    if (slot == nullptr) {
        return;
    }
    if (client != nullptr) {
        if (slot->buffer_id != 0) {
            (void)khr_shm_buffer_destroy(client, slot->buffer_id);
        }
        if (slot->release_tl_id != 0) {
            (void)khr_syncobj_destroy_timeline(client, slot->release_tl_id);
        }
    }
    int drm_fd = (dev != nullptr) ? dev->drm_fd : -1;
    khr_dmp_destroy_timeline(drm_fd, slot->release_handle);
    if (dev != nullptr) {
        khr_dmabuf_slot_destroy(dev, &slot->gfx);
    }
    *slot = (khr_dmabuf_pslot_t){};
}

[[nodiscard]]
static bool khr_dmp_init_slot(khr_gfx_device_t* dev, khr_wl_client_t* client,
                              uint32_t dmabuf_id, uint32_t mgr_id,
                              uint32_t w, uint32_t h, khr_dmabuf_pslot_t* slot) {
    if (dev == nullptr || client == nullptr || slot == nullptr ||
        dmabuf_id == 0 || mgr_id == 0) {
        return false;
    }
    *slot = (khr_dmabuf_pslot_t){};
    if (!khr_dmabuf_slot_init(dev, &slot->gfx, w, h)) {
        return false;
    }
    const khr_dmabuf_image_t* img = &slot->gfx.img;
    if (!khr_dmabuf_import_full(client, dmabuf_id, img->dma_fd, w, h,
                                img->drm_format, img->stride, img->offset,
                                img->modifier, &slot->params_id,
                                &slot->buffer_id)) {
        khr_dmp_retire_slot(dev, client, slot);
        return false;
    }
    slot->release_handle = khr_dmp_create_timeline(dev->drm_fd);
    int rel_fd = -1;
    uint32_t rel_tl = 0;
    if (slot->release_handle == 0 ||
        !khr_dmp_export_fd(dev->drm_fd, slot->release_handle, &rel_fd) ||
        !khr_syncobj_import_timeline(client, mgr_id, rel_fd, &rel_tl)) {
        if (rel_fd >= 0) {
            close(rel_fd);
        }
        khr_dmp_retire_slot(dev, client, slot);
        return false;
    }
    close(rel_fd);
    slot->release_tl_id = rel_tl;
    return true;
}

void khr_dmabuf_present_drop_retired(khr_gfx_device_t* dev,
                                     khr_dmabuf_present_t* p) {
    if (p == nullptr || !p->has_retiring) {
        return;
    }
    for (uint32_t i = 0; i < KHR_DMABUF_PRESENT_SLOTS; i++) {
        khr_dmp_retire_slot(dev, p->client, &p->retiring[i]);
    }
    p->has_retiring = false;
}

[[nodiscard]]
bool khr_dmabuf_present_init(khr_gfx_device_t* dev, khr_wl_client_t* client,
                             uint32_t surface_id, uint32_t w, uint32_t h,
                             khr_dmabuf_present_t* out) {
    if (dev == nullptr || dev->device == VK_NULL_HANDLE || dev->drm_fd < 0 ||
        dev->acquire_sem == VK_NULL_HANDLE || client == nullptr ||
        surface_id == 0 || w == 0 || h == 0 || w > 4'096 || h > 4'096 ||
        out == nullptr) {
        return false;
    }
    *out = (khr_dmabuf_present_t){ .client = client };
    out->surface_id = surface_id;
    out->width = w;
    out->height = h;

    if (!khr_dmabuf_bind(client, &out->dmabuf_id) ||
        !khr_syncobj_bind(client, &out->mgr_id) ||
        !khr_syncobj_get_surface(client, out->mgr_id, surface_id,
                                 &out->sync_surface_id)) {
        khr_dmabuf_present_destroy(dev, out);
        return false;
    }

    /* Acquire timeline: DRM object + one Wayland import for the loop. */
    out->acquire_handle = khr_dmp_create_timeline(dev->drm_fd);
    int acq_fd = -1;
    uint32_t acq_tl = 0;
    if (out->acquire_handle == 0 ||
        !khr_dmp_export_fd(dev->drm_fd, out->acquire_handle, &acq_fd) ||
        !khr_syncobj_import_timeline(client, out->mgr_id, acq_fd, &acq_tl)) {
        if (acq_fd >= 0) {
            close(acq_fd);
        }
        khr_dmabuf_present_destroy(dev, out);
        return false;
    }
    close(acq_fd);
    out->acquire_tl_id = acq_tl;

    for (uint32_t i = 0; i < KHR_DMABUF_PRESENT_SLOTS; i++) {
        if (!khr_dmp_init_slot(dev, client, out->dmabuf_id, out->mgr_id, w, h,
                               &out->slots[i])) {
            khr_dmabuf_present_destroy(dev, out);
            return false;
        }
    }
    return true;
}

[[nodiscard]]
bool khr_dmabuf_present_resize(khr_gfx_device_t* dev, khr_dmabuf_present_t* p,
                               uint32_t w, uint32_t h) {
    if (dev == nullptr || p == nullptr || p->client == nullptr ||
        p->dmabuf_id == 0 || p->mgr_id == 0 || p->sync_surface_id == 0 ||
        w == 0 || h == 0 || w > 4'096 || h > 4'096) {
        return false;
    }
    if (p->width == w && p->height == h) {
        return true;
    }
    khr_dmabuf_pslot_t neu[KHR_DMABUF_PRESENT_SLOTS] = {};
    for (uint32_t i = 0; i < KHR_DMABUF_PRESENT_SLOTS; i++) {
        if (!khr_dmp_init_slot(dev, p->client, p->dmabuf_id, p->mgr_id, w, h,
                               &neu[i])) {
            for (uint32_t j = 0; j < i; j++) {
                khr_dmp_retire_slot(dev, p->client, &neu[j]);
            }
            return false;
        }
    }
    khr_dmabuf_present_drop_retired(dev, p);
    memcpy(p->retiring, p->slots, sizeof(p->slots));
    memcpy(p->slots, neu, sizeof(neu));
    p->has_retiring = true;
    p->width = w;
    p->height = h;
    p->next_slot = 0;
    return true;
}

[[nodiscard]]
uint32_t khr_dmabuf_present_next_free(const khr_dmabuf_present_t* p) {
    if (p == nullptr) {
        return UINT32_MAX;
    }
    for (uint32_t i = 0; i < KHR_DMABUF_PRESENT_SLOTS; i++) {
        uint32_t idx = (p->next_slot + i) % KHR_DMABUF_PRESENT_SLOTS;
        if (!p->slots[idx].busy && !p->slots[idx].failed) {
            return idx;
        }
    }
    return UINT32_MAX;
}

[[nodiscard]]
uint64_t khr_dmabuf_present_sync(khr_gfx_device_t* dev,
                                 khr_dmabuf_present_t* p) {
    if (dev == nullptr || dev->device == VK_NULL_HANDLE ||
        dev->acquire_sem == VK_NULL_HANDLE || p == nullptr ||
        dev->drm_fd < 0 || p->acquire_handle == 0) {
        return 0;
    }
    /* Counter query: MMIO read, never blocks. The point named in the last
     * commit fires at the compositor as soon as this pushes it — a lagging
     * lap only delays display, never shows unfinished pixels. */
    uint64_t done = 0;
    if (vkGetSemaphoreCounterValue(dev->device, dev->acquire_sem, &done) !=
        VK_SUCCESS) {
        return p->last_signaled;
    }
    if (done > p->last_signaled) {
        if (khr_dmp_signal(dev->drm_fd, p->acquire_handle, done)) {
            p->last_signaled = done;
        }
    }
    return p->last_signaled;
}

[[nodiscard]]
bool khr_dmabuf_present_commit_frame(khr_gfx_device_t* dev,
                                     khr_dmabuf_present_t* p,
                                     khr_bda_arena_t* arena, uint64_t frame_no) {
    if (dev == nullptr || p == nullptr || p->client == nullptr ||
        arena == nullptr) {
        return false;
    }
    uint32_t idx = khr_dmabuf_present_next_free(p);
    if (idx == UINT32_MAX) {
        return false;
    }
    khr_dmabuf_pslot_t* slot = &p->slots[idx];
    uint64_t point = dev->acquire_point + 1U;
    if (!khr_dmabuf_slot_render(dev, &slot->gfx, arena, frame_no,
                                dev->acquire_sem, point)) {
        return false;
    }
    dev->acquire_point = point;
    /* Push whatever the GPU already finished; the named point fires whenever
     * its work retires, driven by this and every later sync lap. */
    (void)khr_dmabuf_present_sync(dev, p);
    if (!khr_syncobj_set_points(p->client, p->sync_surface_id,
                                p->acquire_tl_id, point,
                                slot->release_tl_id, slot->release_point + 1U)) {
        return false;
    }
    slot->release_point++;
    /* attach + damage + commit: surface ops, no SHM involved despite the
     * helper's home module. */
    if (!khr_shm_attach_commit(p->client, p->surface_id, slot->buffer_id,
                               p->width, p->height)) {
        return false;
    }
    slot->busy = true;
    p->next_slot = (idx + 1U) % KHR_DMABUF_PRESENT_SLOTS;
    p->frames++;
    return true;
}

[[nodiscard]]
bool khr_dmabuf_present_commit_cards(khr_gfx_device_t* dev,
                                     khr_dmabuf_present_t* p,
                                     VkDeviceAddress cards_addr,
                                     uint32_t card_count) {
    if (dev == nullptr || p == nullptr || p->client == nullptr ||
        cards_addr == 0 || card_count == 0) {
        return false;
    }
    uint32_t idx = khr_dmabuf_present_next_free(p);
    if (idx == UINT32_MAX) {
        return false;
    }
    khr_dmabuf_pslot_t* slot = &p->slots[idx];
    uint64_t point = dev->acquire_point + 1U;
    if (!khr_dmabuf_slot_render_cards(dev, &slot->gfx, cards_addr, card_count,
                                      dev->acquire_sem, point)) {
        return false;
    }
    dev->acquire_point = point;
    (void)khr_dmabuf_present_sync(dev, p);
    if (!khr_syncobj_set_points(p->client, p->sync_surface_id,
                                p->acquire_tl_id, point,
                                slot->release_tl_id, slot->release_point + 1U)) {
        return false;
    }
    slot->release_point++;
    if (!khr_shm_attach_commit(p->client, p->surface_id, slot->buffer_id,
                               p->width, p->height)) {
        return false;
    }
    slot->busy = true;
    p->next_slot = (idx + 1U) % KHR_DMABUF_PRESENT_SLOTS;
    p->frames++;
    return true;
}

[[nodiscard]]
bool khr_dmabuf_present_commit_scene(khr_gfx_device_t* dev,
                                     khr_dmabuf_present_t* p,
                                     VkDeviceAddress cards_addr,
                                     uint32_t card_count,
                                     const khr_plot_pipeline_t* plot,
                                     const khr_plot_push_t* plot_push,
                                     const khr_mesh_pipeline_t* mesh,
                                     const khr_mesh_push_t* mesh_push,
                                     const khr_gizmo_pass_t* gizmo,
                                     uint32_t plot_top_px) {
    if (dev == nullptr || p == nullptr || p->client == nullptr ||
        cards_addr == 0 || card_count == 0) {
        return false;
    }
    uint32_t idx = khr_dmabuf_present_next_free(p);
    if (idx == UINT32_MAX) {
        return false;
    }
    khr_dmabuf_pslot_t* slot = &p->slots[idx];
    uint64_t point = dev->acquire_point + 1U;
    if (!khr_dmabuf_slot_render_scene(dev, &slot->gfx, cards_addr, card_count,
                                      plot, plot_push, mesh, mesh_push, gizmo,
                                      plot_top_px, dev->acquire_sem, point)) {
        return false;
    }
    dev->acquire_point = point;
    (void)khr_dmabuf_present_sync(dev, p);
    if (!khr_syncobj_set_points(p->client, p->sync_surface_id,
                                p->acquire_tl_id, point,
                                slot->release_tl_id, slot->release_point + 1U)) {
        return false;
    }
    slot->release_point++;
    if (!khr_shm_attach_commit(p->client, p->surface_id, slot->buffer_id,
                               p->width, p->height)) {
        return false;
    }
    slot->busy = true;
    p->next_slot = (idx + 1U) % KHR_DMABUF_PRESENT_SLOTS;
    p->frames++;
    return true;
}

uint32_t khr_dmabuf_present_consume(khr_dmabuf_present_t* p,
                                    const uint8_t* data, size_t len) {
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
        if (hdr.size < 8 || offset + hdr.size > len) {
            break;
        }
        const uint8_t* body = data + offset + 8;
        size_t body_len = hdr.size - 8;
        if (hdr.opcode == KHR_WL_BUFFER_EVENT_RELEASE && hdr.size == 8) {
            bool hit = false;
            for (uint32_t i = 0; i < KHR_DMABUF_PRESENT_SLOTS; i++) {
                if (p->slots[i].buffer_id == hdr.object_id &&
                    p->slots[i].busy) {
                    p->slots[i].busy = false;
                    p->releases++;
                    count++;
                    hit = true;
                    break;
                }
            }
            if (!hit && p->has_retiring) {
                for (uint32_t i = 0; i < KHR_DMABUF_PRESENT_SLOTS; i++) {
                    if (p->retiring[i].buffer_id == hdr.object_id &&
                        p->retiring[i].busy) {
                        p->retiring[i].busy = false;
                        p->releases++;
                        count++;
                        break;
                    }
                }
            }
        } else if (hdr.opcode == KHR_DMABUF_PARAMS_EVENT_CREATED &&
                   hdr.size == 12 && body_len >= 4) {
            uint32_t new_id = 0;
            memcpy(&new_id, body, 4);
            for (uint32_t i = 0; i < KHR_DMABUF_PRESENT_SLOTS; i++) {
                if (p->slots[i].params_id == hdr.object_id &&
                    p->slots[i].buffer_id == new_id) {
                    p->slots[i].created = true;
                    p->created_count++;
                    count++;
                    break;
                }
            }
        } else if (hdr.opcode == KHR_DMABUF_PARAMS_EVENT_FAILED &&
                   hdr.size == 8) {
            for (uint32_t i = 0; i < KHR_DMABUF_PRESENT_SLOTS; i++) {
                if (p->slots[i].params_id == hdr.object_id &&
                    !p->slots[i].failed) {
                    p->slots[i].failed = true;
                    p->failed_count++;
                    count++;
                    break;
                }
            }
        }
        offset += hdr.size;
    }
    return count;
}

void khr_dmabuf_present_destroy(khr_gfx_device_t* dev,
                                khr_dmabuf_present_t* p) {
    if (p == nullptr) {
        return;
    }
    khr_dmabuf_present_drop_retired(dev, p);
    khr_wl_client_t* client = p->client;
    for (uint32_t i = 0; i < KHR_DMABUF_PRESENT_SLOTS; i++) {
        khr_dmp_retire_slot(dev, client, &p->slots[i]);
    }
    if (client != nullptr && p->acquire_tl_id != 0) {
        (void)khr_syncobj_destroy_timeline(client, p->acquire_tl_id);
    }
    int drm_fd = (dev != nullptr) ? dev->drm_fd : -1;
    khr_dmp_destroy_timeline(drm_fd, p->acquire_handle);
    *p = (khr_dmabuf_present_t){};
}

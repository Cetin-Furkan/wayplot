#include "test_framework.h"
#include "mock_compositor.h"
#include "khoros/core/topology.h"
#include "khoros/wayland/client.h"
#include "khoros/wayland/dmabuf.h"
#include "khoros/gfx/device.h"
#include "khoros/gfx/dmabuf.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/mman.h>

/* Same open file description on both ends of SCM_RIGHTS? dev+ino must match:
 * this is the zero-copy proof, not just "an fd arrived". */
static bool khr_test_same_file(int a, int b) {
    struct stat sa = {};
    struct stat sb = {};
    if (fstat(a, &sa) != 0 || fstat(b, &sb) != 0) {
        return false;
    }
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

[[nodiscard]]
static bool test_dmabuf_wire_import_fd_passing_body(khr_topology_t* topo_ctx) {
/* `topo` aliases wrapper-owned storage so every TEST_ASSERT failure below
 * still unwinds through the wrapper's destroy: a red test can never strand
 * the worker thread on freed stack (cascade SEGV). Never declare a local
 * named topo or topo_ctx in here. */
#define topo (*topo_ctx)
    mock_compositor_t comp = {};
    TEST_ASSERT(mock_compositor_init(&comp), "mock init failed");

    khr_wl_client_t client = {
        .sock_fd = comp.client_fd,
        .ring = &topo.ring_a,
    };
    TEST_ASSERT(mock_compositor_send_globals(&comp), "send globals failed");
    TEST_ASSERT(mock_compositor_send_sync_done(&comp, KHR_WL_CALLBACK_ID, 1),
                "send sync done failed");
    khr_wl_client_attach_topology(&client, &topo);
    TEST_ASSERT(khr_wl_client_roundtrip(&client), "registry roundtrip failed");
    TEST_ASSERT_NOT_NULL(khr_wl_client_find_global(&client, "zwp_linux_dmabuf_v1"),
                         "dmabuf global missing");

    uint32_t dmabuf_id = 0;
    TEST_ASSERT(khr_dmabuf_bind(&client, &dmabuf_id), "dmabuf bind failed");
    TEST_ASSERT(mock_compositor_drain(&comp) >= 1, "bind request missing");
    TEST_ASSERT_EQ(comp.client_dmabuf_id, dmabuf_id, "dmabuf id mismatch");

    /* Stand-in DMA-BUF: a sealed memfd carrying a recognisable pattern. */
    int mfd = memfd_create("test-dmabuf-plane0", MFD_CLOEXEC);
    TEST_ASSERT(mfd >= 0, "memfd failed");
    const char pattern[4096] = {};
    TEST_ASSERT(write(mfd, pattern, sizeof(pattern)) == (ssize_t)sizeof(pattern),
                "memfd write failed");

    constexpr uint64_t test_mod = 0x0100000000000001ULL; /* X-tiled shaped */
    constexpr uint32_t test_stride = 1024;
    uint32_t buffer_id = 0;
    TEST_ASSERT(khr_dmabuf_import(&client, dmabuf_id, mfd, 256, 256,
                                 0x34325241U /* DRM_FORMAT_ARGB8888 */,
                                 test_stride, 0, test_mod, &buffer_id),
                "dmabuf import failed");
    TEST_ASSERT(mock_compositor_drain(&comp) >= 3, "params/add/immed missing");

    TEST_ASSERT_EQ(comp.dmabuf_params_id != 0, true, "params id missing");
    TEST_ASSERT_EQ(comp.dma_plane_count, 1U, "exactly one plane expected");
    TEST_ASSERT_EQ(comp.dma_planes[0].plane_idx, 0U, "plane index mismatch");
    TEST_ASSERT_EQ(comp.dma_planes[0].offset, 0U, "plane offset mismatch");
    TEST_ASSERT_EQ(comp.dma_planes[0].stride, test_stride, "stride mismatch");
    TEST_ASSERT_EQ(comp.dma_planes[0].modifier_hi, (uint32_t)(test_mod >> 32),
                   "modifier hi mismatch");
    TEST_ASSERT_EQ(comp.dma_planes[0].modifier_lo, (uint32_t)(test_mod & 0xFFFF'FFFFU),
                   "modifier lo mismatch");
    TEST_ASSERT(khr_test_same_file(mfd, comp.dma_planes[0].fd),
                "SCM_RIGHTS fd is not the same open file (copy, not zero-copy)");
    TEST_ASSERT_EQ(comp.dmabuf_buffer_id, buffer_id, "wl_buffer id mismatch");

    close(mfd);
    mock_compositor_destroy(&comp);
#undef topo
    return true;
}

[[nodiscard]]
bool test_dmabuf_wire_import_fd_passing(void) {
    khr_topology_t topo = {};
    if (!khr_topology_init(&topo)) {
        return false;
    }
    bool ok = test_dmabuf_wire_import_fd_passing_body(&topo);
    khr_topology_destroy(&topo);
    return ok;
}

/* `topo` aliases wrapper-owned storage so every TEST_ASSERT failure below
 * still unwinds through the wrapper's destroy: a red test can never strand
 * the worker thread on freed stack (cascade SEGV). Never declare a local
 * named topo or topo_ctx in here. */
static bool test_dmabuf_vulkan_export_and_import_body(khr_topology_t* topo_ctx) {
#define topo (*topo_ctx)
    khr_gfx_device_t dev = {};
    if (!khr_gfx_device_init(&dev, (dev_t)0)) {
        TEST_SKIP("no Vulkan device in this environment");
        return true;
    }

    khr_dmabuf_image_t img = {};
    TEST_ASSERT(khr_dmabuf_image_init(&dev, &img, 256, 256), "export image init failed");
    TEST_ASSERT(khr_dmabuf_image_export(&dev, &img), "DMA-BUF export failed");
    TEST_ASSERT(img.dma_fd >= 0, "exported fd invalid");
    TEST_ASSERT(img.stride >= 256U * 4U, "stride smaller than tight packing");

    mock_compositor_t comp = {};
    TEST_ASSERT(mock_compositor_init(&comp), "mock init failed");

    khr_wl_client_t client = {
        .sock_fd = comp.client_fd,
        .ring = &topo.ring_a,
    };
    TEST_ASSERT(mock_compositor_send_globals(&comp), "send globals failed");
    TEST_ASSERT(mock_compositor_send_sync_done(&comp, KHR_WL_CALLBACK_ID, 1),
                "send sync done failed");
    khr_wl_client_attach_topology(&client, &topo);
    TEST_ASSERT(khr_wl_client_roundtrip(&client), "registry roundtrip failed");

    uint32_t dmabuf_id = 0;
    TEST_ASSERT(khr_dmabuf_bind(&client, &dmabuf_id), "dmabuf bind failed");

    uint32_t buffer_id = 0;
    TEST_ASSERT(khr_dmabuf_import(&client, dmabuf_id, img.dma_fd, img.w, img.h,
                                 img.drm_format, img.stride, img.offset,
                                 img.modifier, &buffer_id),
                "GPU dmabuf import failed");
    TEST_ASSERT(mock_compositor_drain(&comp) >= 4, "bind+params/add/immed missing");
    TEST_ASSERT_EQ(comp.dma_plane_count, 1U, "exactly one plane expected");
    TEST_ASSERT(khr_test_same_file(img.dma_fd, comp.dma_planes[0].fd),
                "exported fd did not survive SCM_RIGHTS intact");
    uint64_t wire_mod = ((uint64_t)comp.dma_planes[0].modifier_hi << 32) |
                        comp.dma_planes[0].modifier_lo;
    TEST_ASSERT_EQ(wire_mod, img.modifier, "driver modifier must cross the wire verbatim");
    TEST_ASSERT_EQ(comp.dma_planes[0].stride, img.stride, "pitch must cross verbatim");

    mock_compositor_destroy(&comp);
#undef topo
    khr_dmabuf_image_destroy(&dev, &img);
    khr_gfx_device_destroy(&dev);
    return true;
}

[[nodiscard]]
bool test_dmabuf_vulkan_export_and_import(void) {
    khr_topology_t topo = {};
    if (!khr_topology_init(&topo)) {
        return false;
    }
    bool ok = test_dmabuf_vulkan_export_and_import_body(&topo);
    khr_topology_destroy(&topo);
    return ok;
}

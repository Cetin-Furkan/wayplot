#include "test_framework.h"
#include "mock_compositor.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <drm/drm.h>
#include <drm/drm_fourcc.h>

static int open_render_node(void) {
    int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    }
    return fd;
}

[[nodiscard]]
bool test_drm_syncobj_create_and_export_fd(void) {
    int drm_fd = open_render_node();
    if (drm_fd < 0) {
        printf("  (Notice: /dev/dri/renderD128 not accessible, skipping live DRM ioctl) ");
        return true;
    }

    /* 1. Create DRM Syncobj */
    struct drm_syncobj_create create = { .flags = 0 };
    TEST_ASSERT_EQ(ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_CREATE, &create), 0, "DRM_IOCTL_SYNCOBJ_CREATE failed");
    TEST_ASSERT(create.handle > 0, "Syncobj handle must be > 0");

    /* 2. Export to FD */
    struct drm_syncobj_handle h2f = { .handle = create.handle, .flags = 0 };
    TEST_ASSERT_EQ(ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &h2f), 0, "DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD failed");
    TEST_ASSERT(h2f.fd >= 0, "Exported syncobj FD must be >= 0");

    /* 3. Cleanup */
    close(h2f.fd);
    struct drm_syncobj_destroy destroy = { .handle = create.handle };
    TEST_ASSERT_EQ(ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy), 0, "DRM_IOCTL_SYNCOBJ_DESTROY failed");
    close(drm_fd);
    return true;
}

[[nodiscard]]
bool test_drm_syncobj_fd_to_handle_roundtrip(void) {
    int drm_fd = open_render_node();
    if (drm_fd < 0) return true;

    /* Create handle 1 */
    struct drm_syncobj_create create = { .flags = 0 };
    TEST_ASSERT_EQ(ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_CREATE, &create), 0, "Create syncobj");

    /* Export to FD */
    struct drm_syncobj_handle h2f = { .handle = create.handle, .flags = 0 };
    TEST_ASSERT_EQ(ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &h2f), 0, "Handle to FD");

    /* Re-import FD into handle 2 */
    struct drm_syncobj_handle f2h = { .fd = h2f.fd, .flags = 0 };
    TEST_ASSERT_EQ(ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &f2h), 0, "FD to Handle");
    TEST_ASSERT(f2h.handle > 0, "Re-imported handle must be valid");

    /* Both handles refer to the same kernel syncobj */
    close(h2f.fd);

    struct drm_syncobj_destroy d1 = { .handle = create.handle };
    ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_DESTROY, &d1);

    struct drm_syncobj_destroy d2 = { .handle = f2h.handle };
    ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_DESTROY, &d2);

    close(drm_fd);
    return true;
}

[[nodiscard]]
bool test_drm_syncobj_timeline_point_signaling(void) {
    int drm_fd = open_render_node();
    if (drm_fd < 0) return true;

    struct drm_syncobj_create create = { .flags = 0 };
    TEST_ASSERT_EQ(ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_CREATE, &create), 0, "Create syncobj");

    uint32_t handle = create.handle;

    /* Signal point 100 */
    uint64_t point = 100;
    struct drm_syncobj_timeline_array sig = {
        .handles = (uint64_t)(uintptr_t)&handle,
        .points = (uint64_t)(uintptr_t)&point,
        .count_handles = 1,
    };
    TEST_ASSERT_EQ(ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &sig), 0, "Signal point 100");

    /* Wait for point 100 with timeout 0 */
    struct drm_syncobj_timeline_wait wait = {
        .handles = (uint64_t)(uintptr_t)&handle,
        .points = (uint64_t)(uintptr_t)&point,
        .timeout_nsec = 0,
        .count_handles = 1,
        .flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
    };
    TEST_ASSERT_EQ(ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &wait), 0, "Wait point 100");

    /* Signal point 200 */
    point = 200;
    TEST_ASSERT_EQ(ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &sig), 0, "Signal point 200");
    TEST_ASSERT_EQ(ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &wait), 0, "Wait point 200");

    struct drm_syncobj_destroy destroy = { .handle = create.handle };
    ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
    close(drm_fd);
    return true;
}

[[nodiscard]]
bool test_drm_syncobj_timeline_wait_timeout(void) {
    int drm_fd = open_render_node();
    if (drm_fd < 0) return true;

    struct drm_syncobj_create create = { .flags = 0 };
    TEST_ASSERT_EQ(ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_CREATE, &create), 0, "Create syncobj");

    /* Wait for unsignaled point 9999 with 1ms timeout */
    uint32_t handle = create.handle;
    uint64_t unsignaled_point = 9999;

    struct timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t timeout_abs_ns = ((int64_t)ts.tv_sec * 1'000'000'000LL) + ts.tv_nsec + 1'000'000LL; /* +1 ms */

    struct drm_syncobj_timeline_wait wait = {
        .handles = (uint64_t)(uintptr_t)&handle,
        .points = (uint64_t)(uintptr_t)&unsignaled_point,
        .timeout_nsec = timeout_abs_ns,
        .count_handles = 1,
        .flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
    };

    int r = ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &wait);
    /* Should time out: returns -1 with ETIME or EBUSY */
    TEST_ASSERT(r < 0 && (errno == ETIME || errno == EBUSY || errno == ETIMEDOUT), "Unsignaled point must time out");

    struct drm_syncobj_destroy destroy = { .handle = create.handle };
    ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
    close(drm_fd);
    return true;
}

[[nodiscard]]
bool test_drm_syncobj_per_image_release_isolation(void) {
    int drm_fd = open_render_node();
    if (drm_fd < 0) return true;

    /* Create 3 independent release timelines for 3 swapchain slots */
    uint32_t handles[3] = {};
    for (int i = 0; i < 3; i++) {
        struct drm_syncobj_create c = { .flags = 0 };
        TEST_ASSERT_EQ(ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_CREATE, &c), 0, "Create slot syncobj");
        handles[i] = c.handle;
    }

    /* Signal slot 0 at point 1 */
    uint64_t point = 1;
    struct drm_syncobj_timeline_array sig = {
        .handles = (uint64_t)(uintptr_t)&handles[0],
        .points = (uint64_t)(uintptr_t)&point,
        .count_handles = 1,
    };
    TEST_ASSERT_EQ(ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &sig), 0, "Signal slot 0");

    /* Verify slot 0 is signaled */
    struct drm_syncobj_timeline_wait wait0 = {
        .handles = (uint64_t)(uintptr_t)&handles[0],
        .points = (uint64_t)(uintptr_t)&point,
        .timeout_nsec = 0,
        .count_handles = 1,
        .flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL,
    };
    TEST_ASSERT_EQ(ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &wait0), 0, "Slot 0 must be signaled");

    /* Verify slots 1 and 2 are NOT signaled (isolation guaranteed) */
    struct drm_syncobj_timeline_wait wait1 = {
        .handles = (uint64_t)(uintptr_t)&handles[1],
        .points = (uint64_t)(uintptr_t)&point,
        .timeout_nsec = 0,
        .count_handles = 1,
        .flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL,
    };
    int r1 = ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &wait1);
    TEST_ASSERT(r1 < 0, "Slot 1 must remain unsignaled");

    struct drm_syncobj_timeline_wait wait2 = {
        .handles = (uint64_t)(uintptr_t)&handles[2],
        .points = (uint64_t)(uintptr_t)&point,
        .timeout_nsec = 0,
        .count_handles = 1,
        .flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL,
    };
    int r2 = ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &wait2);
    TEST_ASSERT(r2 < 0, "Slot 2 must remain unsignaled");

    for (int i = 0; i < 3; i++) {
        struct drm_syncobj_destroy d = { .handle = handles[i] };
        ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_DESTROY, &d);
    }
    close(drm_fd);
    return true;
}

[[nodiscard]]
bool test_dmabuf_plane_parameters_layout(void) {
    /* Test geometry calculation for swapchain buffer layout */
    uint32_t width = 3840;
    uint32_t height = 2160;
    uint32_t bpp = 4; /* 32bpp XRGB8888 */

    uint32_t stride = width * bpp;
    TEST_ASSERT_EQ(stride, 15360U, "stride calculation");
    TEST_ASSERT_EQ(stride % 64, 0U, "stride must be 64-byte aligned for cacheline access");

    size_t total_size = (size_t)stride * height;
    TEST_ASSERT_EQ(total_size, 33177600U, "total buffer size");

    /* Verify page alignment */
    size_t page_aligned = (total_size + 4095) & ~4095UL;
    TEST_ASSERT_GE(page_aligned, total_size, "page alignment bounds");
    TEST_ASSERT_EQ(page_aligned % 4096, 0U, "must be 4 KiB aligned");

    return true;
}

[[nodiscard]]
bool test_dmabuf_export_and_wire_import_integration(void) {
    mock_compositor_t comp = {};
    TEST_ASSERT(mock_compositor_init(&comp), "init mock compositor");
    comp.client_dmabuf_id = 12;

    /* Create dummy DMA-BUF backing memory */
    int dma_fd = memfd_create("test_swapchain_image0", MFD_CLOEXEC);
    TEST_ASSERT(dma_fd >= 0, "memfd_create");
    TEST_ASSERT_EQ(ftruncate(dma_fd, 1920 * 1080 * 4), 0, "ftruncate");

    /* Client sends: create_params -> add -> create_immed */
    khr_wl_msg_buf_t tx = {};
    khr_wl_buf_init(&tx);
    TEST_ASSERT(khr_wl_encode_header(&tx, 12, KHR_DMABUF_CREATE_PARAMS, 12), "create_params");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 40), "params_id 40");
    TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, tx.data, tx.size, -1), "send create_params");
    mock_compositor_drain(&comp);

    khr_wl_buf_init(&tx);
    TEST_ASSERT(khr_wl_encode_header(&tx, 40, KHR_DMABUF_PARAMS_ADD, 28), "params_add");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 0), "plane 0");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 0), "offset 0");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 1920 * 4), "stride");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 0), "mod_hi");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 0), "mod_lo");
    TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, tx.data, tx.size, dma_fd), "send params_add with fd");

    khr_wl_buf_init(&tx);
    TEST_ASSERT(khr_wl_encode_header(&tx, 40, KHR_DMABUF_PARAMS_CREATE_IMMED, 28), "create_immed");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 50), "buffer_id 50");
    TEST_ASSERT(khr_wl_encode_i32(&tx, 1920), "width 1920");
    TEST_ASSERT(khr_wl_encode_i32(&tx, 1080), "height 1080");
    TEST_ASSERT(khr_wl_encode_u32(&tx, DRM_FORMAT_XRGB8888), "format");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 0), "flags");
    TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, tx.data, tx.size, -1), "send create_immed");

    mock_compositor_drain(&comp);
    TEST_ASSERT_EQ(comp.dmabuf_buffer_id, 50U, "buffer_id must be 50");
    TEST_ASSERT_EQ(comp.dma_plane_count, 1U, "must have 1 plane");
    TEST_ASSERT_EQ(comp.dma_planes[0].stride, 1920U * 4, "plane 0 stride mismatch");
    TEST_ASSERT(comp.dma_planes[0].fd >= 0, "mock compositor received valid DMA-BUF FD");

    close(dma_fd);
    mock_compositor_destroy(&comp);
    return true;
}

[[nodiscard]]
bool test_dmabuf_syncobj_full_surface_commit(void) {
    mock_compositor_t comp = {};
    TEST_ASSERT(mock_compositor_init(&comp), "init mock compositor");
    comp.surface_id = 20;
    comp.client_syncobj_mgr_id = 13;

    int sync_fd = memfd_create("test_syncobj_timeline", MFD_CLOEXEC);
    TEST_ASSERT(sync_fd >= 0, "memfd_create");

    khr_wl_msg_buf_t tx = {};
    khr_wl_buf_init(&tx);

    /* 1. import timeline */
    TEST_ASSERT(khr_wl_encode_header(&tx, 13, KHR_SYNCOBJ_MGR_IMPORT_TIMELINE, 12), "import timeline");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 70), "timeline_id 70");
    TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, tx.data, tx.size, sync_fd), "send import timeline");

    /* 2. get syncobj surface */
    khr_wl_buf_init(&tx);
    TEST_ASSERT(khr_wl_encode_header(&tx, 13, KHR_SYNCOBJ_MGR_GET_SURFACE, 16), "get syncobj surface");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 71), "syncobj_surface 71");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 20), "surface 20");
    TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, tx.data, tx.size, -1), "send get_surface");

    /* 3. attach buffer 50 */
    khr_wl_buf_init(&tx);
    TEST_ASSERT(khr_wl_encode_header(&tx, 20, KHR_WL_SURFACE_ATTACH, 20), "attach");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 50), "buffer 50");
    TEST_ASSERT(khr_wl_encode_i32(&tx, 0), "x");
    TEST_ASSERT(khr_wl_encode_i32(&tx, 0), "y");
    TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, tx.data, tx.size, -1), "send attach");

    /* 4. set acquire point = 1000 */
    khr_wl_buf_init(&tx);
    TEST_ASSERT(khr_wl_encode_header(&tx, 71, KHR_SYNCOBJ_SURFACE_SET_ACQUIRE, 20), "set acquire");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 70), "timeline 70");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 0), "hi 0");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 1000), "lo 1000");
    TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, tx.data, tx.size, -1), "send acquire");

    /* 5. set release point = 1001 */
    khr_wl_buf_init(&tx);
    TEST_ASSERT(khr_wl_encode_header(&tx, 71, KHR_SYNCOBJ_SURFACE_SET_RELEASE, 20), "set release");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 70), "timeline 70");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 0), "hi 0");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 1001), "lo 1001");
    TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, tx.data, tx.size, -1), "send release");

    /* 6. surface commit */
    khr_wl_buf_init(&tx);
    TEST_ASSERT(khr_wl_encode_header(&tx, 20, KHR_WL_SURFACE_COMMIT, 8), "commit");
    TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, tx.data, tx.size, -1), "send commit");

    mock_compositor_drain(&comp);
    TEST_ASSERT_EQ(comp.commit_count, 1U, "commit_count must be 1");
    TEST_ASSERT_EQ(comp.last_attached_buffer, 50U, "attached buffer must be 50");
    TEST_ASSERT_EQ(comp.acquire_point, 1000ULL, "acquire_point must be 1000");
    TEST_ASSERT_EQ(comp.release_point, 1001ULL, "release_point must be 1001");

    close(sync_fd);
    mock_compositor_destroy(&comp);
    return true;
}

[[nodiscard]]
bool test_dmabuf_syncobj_timeline_destroy_on_swapchain_retire(void) {
    mock_compositor_t comp = {};
    TEST_ASSERT(mock_compositor_init(&comp), "init mock compositor");

    /* Emit timeline destroy opcode 0 for 3 swapchain release timelines */
    for (uint32_t id = 80; id < 83; id++) {
        comp.timeline_id = id;
        khr_wl_msg_buf_t tx = {};
        khr_wl_buf_init(&tx);
        TEST_ASSERT(khr_wl_encode_header(&tx, id, KHR_SYNCOBJ_TIMELINE_DESTROY, 8), "timeline destroy");
        TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, tx.data, tx.size, -1), "send destroy");
        mock_compositor_drain(&comp);
    }

    TEST_ASSERT_EQ(comp.timeline_destroy_count, 3U, "All 3 timelines must be destroyed to prevent compositor leak");

    mock_compositor_destroy(&comp);
    return true;
}

[[nodiscard]]
bool test_drm_syncobj_boundary_points(void) {
    int drm_fd = open_render_node();
    if (drm_fd < 0) return true;

    struct drm_syncobj_create create = { .flags = 0 };
    TEST_ASSERT_EQ(ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_CREATE, &create), 0, "Create syncobj");
    uint32_t handle = create.handle;

    /* Boundary 1: Point 1 */
    uint64_t p1 = 1;
    struct drm_syncobj_timeline_array sig1 = {
        .handles = (uint64_t)(uintptr_t)&handle,
        .points = (uint64_t)(uintptr_t)&p1,
        .count_handles = 1,
    };
    TEST_ASSERT_EQ(ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &sig1), 0, "Signal point 1");

    /* Boundary 2: Large point near UINT64_MAX */
    uint64_t p_large = 0xFFFFFFFFFFFFF000ULL;
    struct drm_syncobj_timeline_array sig2 = {
        .handles = (uint64_t)(uintptr_t)&handle,
        .points = (uint64_t)(uintptr_t)&p_large,
        .count_handles = 1,
    };
    TEST_ASSERT_EQ(ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &sig2), 0, "Signal large point");

    struct drm_syncobj_timeline_wait wait = {
        .handles = (uint64_t)(uintptr_t)&handle,
        .points = (uint64_t)(uintptr_t)&p_large,
        .timeout_nsec = 0,
        .count_handles = 1,
        .flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL,
    };
    TEST_ASSERT_EQ(ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &wait), 0, "Wait large point");

    struct drm_syncobj_destroy destroy = { .handle = create.handle };
    ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
    close(drm_fd);
    return true;
}

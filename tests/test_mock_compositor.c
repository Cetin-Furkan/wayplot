#include "test_framework.h"
#include "mock_compositor.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

[[nodiscard]]
bool test_mock_compositor_init_destroy(void) {
    mock_compositor_t comp = {};
    TEST_ASSERT(mock_compositor_init(&comp), "mock_compositor_init failed");
    TEST_ASSERT(comp.server_fd >= 0, "server_fd must be valid");
    TEST_ASSERT(comp.client_fd >= 0, "client_fd must be valid");

    /* Verify non-blocking receive timeout is configured */
    struct timeval tv = {};
    socklen_t tv_len = sizeof(tv);
    TEST_ASSERT(getsockopt(comp.server_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, &tv_len) == 0, "getsockopt SO_RCVTIMEO failed");
    TEST_ASSERT(tv.tv_usec > 0 || tv.tv_sec > 0, "SO_RCVTIMEO timeout must be non-zero");

    mock_compositor_destroy(&comp);
    TEST_ASSERT_EQ(comp.server_fd, -1, "server_fd must be -1 after destroy");
    TEST_ASSERT_EQ(comp.client_fd, -1, "client_fd must be -1 after destroy");
    return true;
}

[[nodiscard]]
bool test_mock_compositor_globals_advertisement(void) {
    mock_compositor_t comp = {};
    TEST_ASSERT(mock_compositor_init(&comp), "init failed");

    TEST_ASSERT(mock_compositor_send_globals(&comp), "send_globals failed");

    /* Client side: read stream and verify 6 advertised globals */
    uint8_t rx_buf[2'048] = {};
    ssize_t n = recv(comp.client_fd, rx_buf, sizeof(rx_buf), 0);
    TEST_ASSERT(n > 0, "client must receive globals");

    size_t off = 0;
    uint32_t count = 0;
    bool found_comp = false;
    bool found_xdg = false;
    bool found_dmabuf = false;
    bool found_syncobj = false;

    while (off + 8 <= (size_t)n) {
        khr_wl_msg_header_t hdr = {};
        TEST_ASSERT(khr_wl_decode_header(rx_buf + off, (size_t)n - off, &hdr), "header decode failed");
        TEST_ASSERT_EQ(hdr.object_id, KHR_WL_REGISTRY_ID, "globals must target registry");
        TEST_ASSERT_EQ(hdr.opcode, KHR_WL_REGISTRY_EVENT_GLOBAL, "opcode must be global event");

        size_t p_off = off + 8;
        uint32_t name = 0;
        const char* iface = nullptr;
        uint32_t iface_len = 0;
        uint32_t ver = 0;

        TEST_ASSERT(khr_wl_decode_u32(rx_buf, (size_t)n, &p_off, &name), "name decode failed");
        TEST_ASSERT(khr_wl_decode_string(rx_buf, (size_t)n, &p_off, &iface, &iface_len), "string decode failed");
        TEST_ASSERT(khr_wl_decode_u32(rx_buf, (size_t)n, &p_off, &ver), "version decode failed");

        if (strcmp(iface, "wl_compositor") == 0) {
            found_comp = true;
            TEST_ASSERT_EQ(ver, 4U, "wl_compositor version must be 4");
        } else if (strcmp(iface, "xdg_wm_base") == 0) {
            found_xdg = true;
            TEST_ASSERT_EQ(ver, 3U, "xdg_wm_base version must be 3");
        } else if (strcmp(iface, "zwp_linux_dmabuf_v1") == 0) {
            found_dmabuf = true;
            TEST_ASSERT_EQ(ver, 4U, "zwp_linux_dmabuf_v1 version must be 4");
        } else if (strcmp(iface, "wp_linux_drm_syncobj_manager_v1") == 0) {
            found_syncobj = true;
            TEST_ASSERT_EQ(ver, 1U, "wp_linux_drm_syncobj_manager_v1 version must be 1");
        }

        count++;
        off += hdr.size;
    }

    TEST_ASSERT_EQ(count, 6U, "must have received exactly 6 globals");
    TEST_ASSERT(found_comp, "wl_compositor must be present");
    TEST_ASSERT(found_xdg, "xdg_wm_base must be present");
    TEST_ASSERT(found_dmabuf, "zwp_linux_dmabuf_v1 must be present");
    TEST_ASSERT(found_syncobj, "wp_linux_drm_syncobj_manager_v1 must be present");

    mock_compositor_destroy(&comp);
    return true;
}

[[nodiscard]]
bool test_mock_compositor_client_bind_tracking(void) {
    mock_compositor_t comp = {};
    TEST_ASSERT(mock_compositor_init(&comp), "init failed");

    /* Client binds wl_compositor (id=10), xdg_wm_base (id=11), zwp_linux_dmabuf_v1 (id=12), wp_linux_drm_syncobj_manager_v1 (id=13) */
    khr_wl_msg_buf_t tx = {};
    khr_wl_buf_init(&tx);

    /* 1. bind wl_compositor */
    uint32_t s_len = (uint32_t)strlen("wl_compositor") + 1;
    uint32_t s_pad = khr_wl_pad4(s_len);
    TEST_ASSERT(khr_wl_encode_header(&tx, KHR_WL_REGISTRY_ID, KHR_WL_REGISTRY_BIND, (uint16_t)(8 + 4 + 4 + s_pad + 4 + 4)), "encode header");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 1), "name");
    TEST_ASSERT(khr_wl_encode_string(&tx, "wl_compositor"), "iface");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 4), "version");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 10), "new_id");

    /* 2. bind xdg_wm_base */
    s_len = (uint32_t)strlen("xdg_wm_base") + 1;
    s_pad = khr_wl_pad4(s_len);
    TEST_ASSERT(khr_wl_encode_header(&tx, KHR_WL_REGISTRY_ID, KHR_WL_REGISTRY_BIND, (uint16_t)(8 + 4 + 4 + s_pad + 4 + 4)), "encode header");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 2), "name");
    TEST_ASSERT(khr_wl_encode_string(&tx, "xdg_wm_base"), "iface");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 3), "version");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 11), "new_id");

    /* 3. bind zwp_linux_dmabuf_v1 */
    s_len = (uint32_t)strlen("zwp_linux_dmabuf_v1") + 1;
    s_pad = khr_wl_pad4(s_len);
    TEST_ASSERT(khr_wl_encode_header(&tx, KHR_WL_REGISTRY_ID, KHR_WL_REGISTRY_BIND, (uint16_t)(8 + 4 + 4 + s_pad + 4 + 4)), "encode header");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 3), "name");
    TEST_ASSERT(khr_wl_encode_string(&tx, "zwp_linux_dmabuf_v1"), "iface");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 4), "version");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 12), "new_id");

    /* 4. bind wp_linux_drm_syncobj_manager_v1 */
    s_len = (uint32_t)strlen("wp_linux_drm_syncobj_manager_v1") + 1;
    s_pad = khr_wl_pad4(s_len);
    TEST_ASSERT(khr_wl_encode_header(&tx, KHR_WL_REGISTRY_ID, KHR_WL_REGISTRY_BIND, (uint16_t)(8 + 4 + 4 + s_pad + 4 + 4)), "encode header");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 4), "name");
    TEST_ASSERT(khr_wl_encode_string(&tx, "wp_linux_drm_syncobj_manager_v1"), "iface");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 1), "version");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 13), "new_id");

    TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, tx.data, tx.size, -1), "send binds failed");

    uint32_t drained = mock_compositor_drain(&comp);
    TEST_ASSERT_EQ(drained, 4U, "server must drain 4 bind messages");

    TEST_ASSERT_EQ(comp.client_compositor_id, 10U, "compositor id tracking mismatch");
    TEST_ASSERT_EQ(comp.client_wm_base_id, 11U, "wm_base id tracking mismatch");
    TEST_ASSERT_EQ(comp.client_dmabuf_id, 12U, "dmabuf id tracking mismatch");
    TEST_ASSERT_EQ(comp.client_syncobj_mgr_id, 13U, "syncobj mgr id tracking mismatch");

    mock_compositor_destroy(&comp);
    return true;
}

[[nodiscard]]
bool test_mock_compositor_surface_creation_and_commit(void) {
    mock_compositor_t comp = {};
    TEST_ASSERT(mock_compositor_init(&comp), "init failed");
    comp.client_compositor_id = 10;

    /* Client creates surface id=20, attaches buffer id=30, and commits */
    khr_wl_msg_buf_t tx = {};
    khr_wl_buf_init(&tx);

    /* wl_compositor.create_surface(new_id=20) */
    TEST_ASSERT(khr_wl_encode_header(&tx, 10, 0, 12), "create_surface");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 20), "new_id 20");

    /* wl_surface.attach(buffer=30, x=0, y=0) */
    TEST_ASSERT(khr_wl_encode_header(&tx, 20, KHR_WL_SURFACE_ATTACH, 20), "attach");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 30), "buffer 30");
    TEST_ASSERT(khr_wl_encode_i32(&tx, 0), "x 0");
    TEST_ASSERT(khr_wl_encode_i32(&tx, 0), "y 0");

    /* wl_surface.commit() */
    TEST_ASSERT(khr_wl_encode_header(&tx, 20, KHR_WL_SURFACE_COMMIT, 8), "commit");

    TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, tx.data, tx.size, -1), "send failed");

    uint32_t drained = mock_compositor_drain(&comp);
    TEST_ASSERT_EQ(drained, 3U, "expected 3 requests drained");
    TEST_ASSERT_EQ(comp.surface_id, 20U, "surface_id mismatch");
    TEST_ASSERT_EQ(comp.last_attached_buffer, 30U, "last_attached_buffer mismatch");
    TEST_ASSERT_EQ(comp.attach_count, 1U, "attach_count mismatch");
    TEST_ASSERT_EQ(comp.commit_count, 1U, "commit_count mismatch");

    mock_compositor_destroy(&comp);
    return true;
}

[[nodiscard]]
bool test_mock_compositor_xdg_lifecycle_and_configure(void) {
    mock_compositor_t comp = {};
    TEST_ASSERT(mock_compositor_init(&comp), "init failed");
    comp.client_wm_base_id = 11;
    comp.surface_id = 20;

    /* 1. Client creates xdg_surface (id=21) and xdg_toplevel (id=22) */
    khr_wl_msg_buf_t tx = {};
    khr_wl_buf_init(&tx);

    /* xdg_wm_base.get_xdg_surface(new_id=21, surface=20) */
    TEST_ASSERT(khr_wl_encode_header(&tx, 11, KHR_XDG_WM_BASE_GET_XDG_SURFACE, 16), "get_xdg_surface");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 21), "new_id 21");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 20), "surface 20");

    /* xdg_surface.get_toplevel(new_id=22) */
    TEST_ASSERT(khr_wl_encode_header(&tx, 21, KHR_XDG_SURFACE_GET_TOPLEVEL, 12), "get_toplevel");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 22), "new_id 22");

    TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, tx.data, tx.size, -1), "send toplevel setup");
    mock_compositor_drain(&comp);
    TEST_ASSERT_EQ(comp.xdg_surface_id, 21U, "xdg_surface_id mismatch");
    TEST_ASSERT_EQ(comp.xdg_toplevel_id, 22U, "xdg_toplevel_id mismatch");

    /* 2. Mock Compositor emits configure event: width=1920, height=1080, serial=9988 */
    TEST_ASSERT(mock_compositor_send_xdg_configure(&comp, 22, 21, 1920, 1080, 9988), "send_xdg_configure");

    /* 3. Client reads configure events and sends ack_configure(9988) */
    uint8_t c_buf[256] = {};
    ssize_t n = recv(comp.client_fd, c_buf, sizeof(c_buf), 0);
    TEST_ASSERT(n >= 32, "client should receive toplevel + surface configure");

    /* Verify surface configure serial in client buffer */
    khr_wl_msg_buf_t ack_tx = {};
    khr_wl_buf_init(&ack_tx);
    TEST_ASSERT(khr_wl_encode_header(&ack_tx, 21, KHR_XDG_SURFACE_ACK_CONFIGURE, 12), "ack_configure");
    TEST_ASSERT(khr_wl_encode_u32(&ack_tx, 9988), "serial 9988");
    TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, ack_tx.data, ack_tx.size, -1), "send ack");

    mock_compositor_drain(&comp);
    TEST_ASSERT_EQ(comp.ack_configure_count, 1U, "ack_configure_count mismatch");
    TEST_ASSERT_EQ(comp.last_ack_serial, 9988U, "last_ack_serial mismatch");

    mock_compositor_destroy(&comp);
    return true;
}

[[nodiscard]]
bool test_mock_compositor_ping_pong_keepalive(void) {
    mock_compositor_t comp = {};
    TEST_ASSERT(mock_compositor_init(&comp), "init failed");
    comp.client_wm_base_id = 11;

    /* Compositor sends ping(serial=424242) */
    TEST_ASSERT(mock_compositor_send_ping(&comp, 11, 424242), "send_ping");

    /* Client receives ping */
    uint8_t p_buf[64] = {};
    ssize_t n = recv(comp.client_fd, p_buf, sizeof(p_buf), 0);
    TEST_ASSERT_EQ(n, 12, "expected 12-byte ping message");

    khr_wl_msg_header_t hdr = {};
    TEST_ASSERT(khr_wl_decode_header(p_buf, (size_t)n, &hdr), "decode ping hdr");
    TEST_ASSERT_EQ(hdr.object_id, 11U, "target wm_base id");
    TEST_ASSERT_EQ(hdr.opcode, KHR_XDG_WM_BASE_EVENT_PING, "ping opcode");

    size_t off = 8;
    uint32_t serial = 0;
    TEST_ASSERT(khr_wl_decode_u32(p_buf, (size_t)n, &off, &serial), "decode serial");
    TEST_ASSERT_EQ(serial, 424242U, "serial mismatch");

    /* Client replies with pong(424242) */
    khr_wl_msg_buf_t pong_tx = {};
    khr_wl_buf_init(&pong_tx);
    TEST_ASSERT(khr_wl_encode_header(&pong_tx, 11, KHR_XDG_WM_BASE_PONG, 12), "pong hdr");
    TEST_ASSERT(khr_wl_encode_u32(&pong_tx, serial), "pong serial");
    TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, pong_tx.data, pong_tx.size, -1), "send pong");

    mock_compositor_drain(&comp);
    TEST_ASSERT_EQ(comp.pong_count, 1U, "pong_count mismatch");
    TEST_ASSERT_EQ(comp.last_pong_serial, 424242U, "last_pong_serial mismatch");

    mock_compositor_destroy(&comp);
    return true;
}

[[nodiscard]]
bool test_mock_compositor_dmabuf_fd_import(void) {
    mock_compositor_t comp = {};
    TEST_ASSERT(mock_compositor_init(&comp), "init failed");
    comp.client_dmabuf_id = 12;

    /* Create dummy DMA-BUF backing file descriptor via memfd_create */
    int mem_fd = memfd_create("mock_dmabuf_plane0", MFD_CLOEXEC);
    TEST_ASSERT(mem_fd >= 0, "memfd_create failed");
    TEST_ASSERT(ftruncate(mem_fd, 1920 * 1080 * 4) == 0, "ftruncate failed");

    khr_wl_msg_buf_t tx = {};
    khr_wl_buf_init(&tx);

    /* 1. zwp_linux_dmabuf_v1.create_params(new_id=40) */
    TEST_ASSERT(khr_wl_encode_header(&tx, 12, KHR_DMABUF_CREATE_PARAMS, 12), "create_params");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 40), "params id 40");
    TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, tx.data, tx.size, -1), "send create_params");

    mock_compositor_drain(&comp);
    TEST_ASSERT_EQ(comp.dmabuf_params_id, 40U, "params_id tracking mismatch");

    /* 2. zwp_linux_buffer_params_v1.add(fd, plane=0, offset=0, stride=7680, mod_hi=0, mod_lo=0) */
    khr_wl_buf_init(&tx);
    TEST_ASSERT(khr_wl_encode_header(&tx, 40, KHR_DMABUF_PARAMS_ADD, 28), "params.add");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 0), "plane_idx 0");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 0), "offset 0");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 7680), "stride 7680");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 0), "mod_hi 0");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 0), "mod_lo 0");

    /* Send with SCM_RIGHTS FD! */
    TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, tx.data, tx.size, mem_fd), "send params.add with fd");

    /* 3. zwp_linux_buffer_params_v1.create_immed(buffer_id=50, w=1920, h=1080, format=0x34325258, flags=0) */
    khr_wl_buf_init(&tx);
    TEST_ASSERT(khr_wl_encode_header(&tx, 40, KHR_DMABUF_PARAMS_CREATE_IMMED, 28), "create_immed");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 50), "buffer_id 50");
    TEST_ASSERT(khr_wl_encode_i32(&tx, 1920), "width 1920");
    TEST_ASSERT(khr_wl_encode_i32(&tx, 1080), "height 1080");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 0x34325258), "format DRM_FORMAT_XRGB8888");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 0), "flags 0");

    TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, tx.data, tx.size, -1), "send create_immed");

    uint32_t drained = mock_compositor_drain(&comp);
    TEST_ASSERT(drained >= 2, "must drain params.add and create_immed");

    TEST_ASSERT_EQ(comp.dma_plane_count, 1U, "expected 1 dma plane");
    TEST_ASSERT(comp.dma_planes[0].fd >= 0, "mock compositor must hold received SCM_RIGHTS FD");
    TEST_ASSERT_EQ(comp.dma_planes[0].stride, 7680U, "stride mismatch");
    TEST_ASSERT_EQ(comp.dmabuf_buffer_id, 50U, "buffer_id mismatch");
    TEST_ASSERT_EQ(comp.dma_width, 1920, "width mismatch");
    TEST_ASSERT_EQ(comp.dma_height, 1080, "height mismatch");
    TEST_ASSERT_EQ(comp.dma_format, 0x34325258U, "format mismatch");

    close(mem_fd);
    mock_compositor_destroy(&comp);
    return true;
}

[[nodiscard]]
bool test_mock_compositor_drm_syncobj_timeline_import_and_points(void) {
    mock_compositor_t comp = {};
    TEST_ASSERT(mock_compositor_init(&comp), "init failed");
    comp.client_syncobj_mgr_id = 13;
    comp.surface_id = 20;

    /* Create mock syncobj FD */
    int sync_fd = memfd_create("mock_drm_syncobj", MFD_CLOEXEC);
    TEST_ASSERT(sync_fd >= 0, "memfd_create syncobj failed");

    khr_wl_msg_buf_t tx = {};
    khr_wl_buf_init(&tx);

    /* 1. wp_linux_drm_syncobj_manager_v1.import_timeline(timeline_id=60) + SCM_RIGHTS */
    TEST_ASSERT(khr_wl_encode_header(&tx, 13, KHR_SYNCOBJ_MGR_IMPORT_TIMELINE, 12), "import_timeline");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 60), "timeline_id 60");
    TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, tx.data, tx.size, sync_fd), "send import_timeline with fd");

    /* 2. wp_linux_drm_syncobj_manager_v1.get_surface(syncobj_surface_id=61, surface=20) */
    khr_wl_buf_init(&tx);
    TEST_ASSERT(khr_wl_encode_header(&tx, 13, KHR_SYNCOBJ_MGR_GET_SURFACE, 16), "get_surface");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 61), "syncobj_surface 61");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 20), "surface 20");
    TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, tx.data, tx.size, -1), "send get_surface");

    /* 3. wp_linux_drm_syncobj_surface_v1.set_acquire_point(timeline=60, point_hi=0x12, point_lo=0x345678) */
    khr_wl_buf_init(&tx);
    TEST_ASSERT(khr_wl_encode_header(&tx, 61, KHR_SYNCOBJ_SURFACE_SET_ACQUIRE, 20), "set_acquire");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 60), "timeline 60");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 0x12), "point_hi 0x12");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 0x345678), "point_lo 0x345678");
    TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, tx.data, tx.size, -1), "send set_acquire");

    /* 4. wp_linux_drm_syncobj_surface_v1.set_release_point(timeline=60, point_hi=0x9A, point_lo=0xBCDE00) */
    khr_wl_buf_init(&tx);
    TEST_ASSERT(khr_wl_encode_header(&tx, 61, KHR_SYNCOBJ_SURFACE_SET_RELEASE, 20), "set_release");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 60), "timeline 60");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 0x9A), "point_hi 0x9A");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 0xBCDE00), "point_lo 0xBCDE00");
    TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, tx.data, tx.size, -1), "send set_release");

    uint32_t drained = mock_compositor_drain(&comp);
    TEST_ASSERT_EQ(drained, 4U, "expected 4 syncobj requests drained");

    TEST_ASSERT_EQ(comp.timeline_id, 60U, "timeline_id mismatch");
    TEST_ASSERT(comp.syncobj_timeline_fd >= 0, "mock compositor must hold timeline FD");
    TEST_ASSERT_EQ(comp.syncobj_surface_id, 61U, "syncobj_surface_id mismatch");

    uint64_t expected_acquire = ((uint64_t)0x12 << 32) | 0x345678ULL;
    uint64_t expected_release = ((uint64_t)0x9A << 32) | 0xBCDE00ULL;

    TEST_ASSERT_EQ(comp.acquire_timeline_id, 60U, "acquire timeline id mismatch");
    TEST_ASSERT_EQ(comp.acquire_point, expected_acquire, "acquire point mismatch");
    TEST_ASSERT_EQ(comp.release_timeline_id, 60U, "release timeline id mismatch");
    TEST_ASSERT_EQ(comp.release_point, expected_release, "release point mismatch");

    close(sync_fd);
    mock_compositor_destroy(&comp);
    return true;
}

[[nodiscard]]
bool test_mock_compositor_timeline_destroy_lifecycle(void) {
    mock_compositor_t comp = {};
    TEST_ASSERT(mock_compositor_init(&comp), "init failed");
    comp.timeline_id = 60;

    /* wp_linux_drm_syncobj_timeline_v1.destroy() -> Opcode 0, size 8 */
    khr_wl_msg_buf_t tx = {};
    khr_wl_buf_init(&tx);
    TEST_ASSERT(khr_wl_encode_header(&tx, 60, KHR_SYNCOBJ_TIMELINE_DESTROY, 8), "timeline destroy");
    TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, tx.data, tx.size, -1), "send destroy");

    mock_compositor_drain(&comp);
    TEST_ASSERT_EQ(comp.timeline_destroy_count, 1U, "timeline_destroy_count must be 1");

    mock_compositor_destroy(&comp);
    return true;
}

[[nodiscard]]
bool test_mock_compositor_multi_plane_dmabuf(void) {
    mock_compositor_t comp = {};
    TEST_ASSERT(mock_compositor_init(&comp), "init failed");
    comp.dmabuf_params_id = 40;

    int fd0 = memfd_create("plane0_y", MFD_CLOEXEC);
    int fd1 = memfd_create("plane1_uv", MFD_CLOEXEC);
    TEST_ASSERT(fd0 >= 0 && fd1 >= 0, "memfd_create failed");

    khr_wl_msg_buf_t tx = {};
    khr_wl_buf_init(&tx);

    /* Plane 0 (Y): plane_idx=0, offset=0, stride=1920 */
    TEST_ASSERT(khr_wl_encode_header(&tx, 40, KHR_DMABUF_PARAMS_ADD, 28), "plane 0");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 0), "idx 0");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 0), "off 0");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 1920), "stride 1920");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 0), "mod_hi");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 0), "mod_lo");
    TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, tx.data, tx.size, fd0), "send plane 0");

    /* Plane 1 (UV): plane_idx=1, offset=1920*1080, stride=1920 */
    khr_wl_buf_init(&tx);
    TEST_ASSERT(khr_wl_encode_header(&tx, 40, KHR_DMABUF_PARAMS_ADD, 28), "plane 1");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 1), "idx 1");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 1920 * 1080), "off 1920*1080");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 1920), "stride 1920");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 0), "mod_hi");
    TEST_ASSERT(khr_wl_encode_u32(&tx, 0), "mod_lo");
    TEST_ASSERT(mock_client_send_msg_with_fd(comp.client_fd, tx.data, tx.size, fd1), "send plane 1");

    uint32_t drained = mock_compositor_drain(&comp);
    TEST_ASSERT_EQ(drained, 2U, "expected 2 planes drained");
    TEST_ASSERT_EQ(comp.dma_plane_count, 2U, "dma_plane_count must be 2");
    TEST_ASSERT_EQ(comp.dma_planes[0].plane_idx, 0U, "plane 0 idx");
    TEST_ASSERT_EQ(comp.dma_planes[1].plane_idx, 1U, "plane 1 idx");
    TEST_ASSERT_EQ(comp.dma_planes[1].offset, 1920U * 1080U, "plane 1 offset");

    close(fd0);
    close(fd1);
    mock_compositor_destroy(&comp);
    return true;
}

[[nodiscard]]
bool test_mock_compositor_buffer_release_event(void) {
    mock_compositor_t comp = {};
    TEST_ASSERT(mock_compositor_init(&comp), "init failed");

    /* Compositor sends buffer release event for buffer_id=50 */
    TEST_ASSERT(mock_compositor_send_buffer_release(&comp, 50), "send release");

    uint8_t rx[64] = {};
    ssize_t n = recv(comp.client_fd, rx, sizeof(rx), 0);
    TEST_ASSERT_EQ(n, 8, "buffer release is exactly 8 bytes (header only)");

    khr_wl_msg_header_t hdr = {};
    TEST_ASSERT(khr_wl_decode_header(rx, (size_t)n, &hdr), "decode release");
    TEST_ASSERT_EQ(hdr.object_id, 50U, "buffer object_id mismatch");
    TEST_ASSERT_EQ(hdr.opcode, KHR_WL_BUFFER_EVENT_RELEASE, "release opcode mismatch");
    TEST_ASSERT_EQ(hdr.size, 8U, "release size mismatch");

    mock_compositor_destroy(&comp);
    return true;
}

[[nodiscard]]
bool test_mock_compositor_malformed_request_resilience(void) {
    mock_compositor_t comp = {};
    TEST_ASSERT(mock_compositor_init(&comp), "init failed");

    /* 1. Truncated header (only 5 bytes) */
    uint8_t trash[5] = { 0x01, 0x00, 0x00, 0x00, 0x02 };
    send(comp.client_fd, trash, sizeof(trash), MSG_NOSIGNAL);
    uint32_t drained = mock_compositor_drain(&comp);
    TEST_ASSERT_EQ(drained, 0U, "truncated header must yield 0 decoded messages");

    /* 2. Header with size > actual payload */
    khr_wl_msg_buf_t bad_tx = {};
    khr_wl_buf_init(&bad_tx);
    (void)khr_wl_encode_header(&bad_tx, 1, 0, 100); /* Claims size 100, but buffer is only 8 bytes */
    send(comp.client_fd, bad_tx.data, bad_tx.size, MSG_NOSIGNAL);
    drained = mock_compositor_drain(&comp);
    TEST_ASSERT_EQ(drained, 0U, "undersized payload must yield 0 decoded messages");

    mock_compositor_destroy(&comp);
    return true;
}

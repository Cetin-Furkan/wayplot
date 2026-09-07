#include "test_framework.h"
#include "mock_compositor.h"
#include "khoros/core/topology.h"
#include "khoros/wayland/client.h"
#include "khoros/wayland/xdg.h"
#include "khoros/wayland/syncobj.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <drm/drm.h>

static bool khr_test_same_file(int a, int b) {
    struct stat sa = {};
    struct stat sb = {};
    if (fstat(a, &sa) != 0 || fstat(b, &sb) != 0) {
        return false;
    }
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

static int khr_test_open_render(void) {
    return open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
}

/*
 * Full explicit-sync lifecycle on the mock through production paths:
 * manager bind, XDG toplevel (real surface id), 3-slot per-image timelines
 * with 64-bit points (hi/lo split incl. high-word values), destroy-on-retire
 * plus re-import. Mirrors the swapchain discipline: one timeline per slot,
 * retired before replacement.
 */
[[nodiscard]]
static bool test_syncobj_wire_full_lifecycle_body(khr_topology_t* topo_ctx) {
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
    TEST_ASSERT_NOT_NULL(khr_wl_client_find_global(&client, "wp_linux_drm_syncobj_manager_v1"),
                         "syncobj manager global missing");

    /* Real surface id via the XDG stack (the syncobj endpoint wraps it). */
    khr_xdg_shell_t shell = {};
    khr_xdg_init(&shell);
    TEST_ASSERT(khr_xdg_bind(&client, &shell), "xdg bind failed");
    TEST_ASSERT(khr_xdg_create_toplevel(&client, &shell, "Khoros", "khoros-engine"),
                "create toplevel failed");

    uint32_t mgr_id = 0;
    TEST_ASSERT(khr_syncobj_bind(&client, &mgr_id), "syncobj bind failed");
    TEST_ASSERT(mock_compositor_drain(&comp) >= 4, "bind/create requests missing");
    TEST_ASSERT_EQ(comp.client_syncobj_mgr_id, mgr_id, "manager id mismatch");

    /* Three swapchain slots, three independent timelines. */
    uint32_t timelines[3] = {};
    int fds[3] = { -1, -1, -1 };
    for (uint32_t i = 0; i < 3; i++) {
        fds[i] = memfd_create("test-syncobj-timeline", MFD_CLOEXEC);
        TEST_ASSERT(fds[i] >= 0, "memfd failed");
        TEST_ASSERT(khr_syncobj_import_timeline(&client, mgr_id, fds[i], &timelines[i]),
                    "import timeline failed");
    }
    TEST_ASSERT(mock_compositor_drain(&comp) >= 3, "import requests missing");
    TEST_ASSERT(timelines[0] != timelines[1] && timelines[1] != timelines[2] &&
                timelines[0] != timelines[2],
                "per-image timelines must be distinct objects");
    TEST_ASSERT(khr_test_same_file(fds[2], comp.syncobj_timeline_fd),
                "last imported fd must cross SCM_RIGHTS intact");

    uint32_t surface_id = 0;
    TEST_ASSERT(khr_syncobj_get_surface(&client, mgr_id, shell.surface_id, &surface_id),
                "get_surface failed");
    TEST_ASSERT(mock_compositor_drain(&comp) >= 1, "get_surface request missing");
    TEST_ASSERT_EQ(comp.syncobj_surface_id, surface_id, "syncobj surface id mismatch");

    /* Points with high words set: the hi/lo split must roundtrip exactly. */
    for (uint32_t i = 0; i < 3; i++) {
        uint64_t acquire = ((uint64_t)(i + 1) << 32) | 0xA0U;
        uint64_t release = ((uint64_t)(i + 7) << 32) | 0xB0U;
        TEST_ASSERT(khr_syncobj_set_points(&client, surface_id, timelines[i], acquire,
                                           timelines[i], release),
                    "set_points failed");
        TEST_ASSERT(mock_compositor_drain(&comp) >= 2, "point requests missing");
        TEST_ASSERT_EQ(comp.acquire_timeline_id, timelines[i], "acquire timeline mismatch");
        TEST_ASSERT_EQ(comp.acquire_point, acquire, "acquire point hi/lo mismatch");
        TEST_ASSERT_EQ(comp.release_timeline_id, timelines[i], "release timeline mismatch");
        TEST_ASSERT_EQ(comp.release_point, release, "release point hi/lo mismatch");
    }

    /* Retire: destroy every timeline before any replacement exists. */
    for (uint32_t i = 0; i < 3; i++) {
        TEST_ASSERT(khr_syncobj_destroy_timeline(&client, timelines[i]),
                    "destroy timeline failed");
    }
    TEST_ASSERT(mock_compositor_drain(&comp) >= 3, "destroy requests missing");
    TEST_ASSERT_EQ(comp.timeline_destroy_count, 3U, "all timelines must be destroyed");

    /* Post-retire import works with a fresh id (no zombie reuse). */
    int rfd = memfd_create("test-syncobj-retire", MFD_CLOEXEC);
    TEST_ASSERT(rfd >= 0, "memfd failed");
    uint32_t fresh = 0;
    TEST_ASSERT(khr_syncobj_import_timeline(&client, mgr_id, rfd, &fresh),
                "post-retire import failed");
    TEST_ASSERT(mock_compositor_drain(&comp) >= 1, "re-import request missing");
    TEST_ASSERT(fresh != timelines[0] && fresh != timelines[1] && fresh != timelines[2],
                "re-import must mint a fresh id");

    for (uint32_t i = 0; i < 3; i++) {
        close(fds[i]);
    }
    close(rfd);
    mock_compositor_destroy(&comp);
#undef topo
    return true;
}

[[nodiscard]]
bool test_syncobj_wire_full_lifecycle(void) {
    khr_topology_t topo = {};
    if (!khr_topology_init(&topo)) {
        return false;
    }
    bool ok = test_syncobj_wire_full_lifecycle_body(&topo);
    khr_topology_destroy(&topo);
    return ok;
}

[[nodiscard]]
static bool test_syncobj_drm_fd_import_body(khr_topology_t* topo_ctx) {
#define topo (*topo_ctx)
    int drm_fd = khr_test_open_render();
    if (drm_fd < 0) {
        TEST_SKIP("no render node in this environment");
    }
    struct drm_syncobj_create create = { .flags = 0 };
    if (ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_CREATE, &create) != 0 || create.handle == 0) {
        close(drm_fd);
        TEST_SKIP("syncobj create unsupported");
    }
    struct drm_syncobj_handle h2f = { .handle = create.handle, .flags = 0 };
    if (ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &h2f) != 0 || h2f.fd < 0) {
        struct drm_syncobj_destroy dd = { .handle = create.handle };
        (void)ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_DESTROY, &dd);
        close(drm_fd);
        TEST_SKIP("syncobj export unsupported");
    }

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

    uint32_t mgr_id = 0;
    TEST_ASSERT(khr_syncobj_bind(&client, &mgr_id), "syncobj bind failed");

    uint32_t timeline_id = 0;
    TEST_ASSERT(khr_syncobj_import_timeline(&client, mgr_id, h2f.fd, &timeline_id),
                "drm syncobj import failed");
    TEST_ASSERT(mock_compositor_drain(&comp) >= 2, "bind+import requests missing");
    TEST_ASSERT_EQ(comp.timeline_id, timeline_id, "timeline id mismatch");
    TEST_ASSERT(khr_test_same_file(h2f.fd, comp.syncobj_timeline_fd),
                "kernel syncobj fd must cross SCM_RIGHTS intact");

    close(h2f.fd);
    struct drm_syncobj_destroy destroy = { .handle = create.handle };
    (void)ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
    close(drm_fd);
    mock_compositor_destroy(&comp);
#undef topo
    return true;
}

[[nodiscard]]
bool test_syncobj_drm_fd_import(void) {
    khr_topology_t topo = {};
    if (!khr_topology_init(&topo)) {
        return false;
    }
    bool ok = test_syncobj_drm_fd_import_body(&topo);
    khr_topology_destroy(&topo);
    return ok;
}

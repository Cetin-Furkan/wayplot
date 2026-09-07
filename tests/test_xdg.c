#include "test_framework.h"
#include "mock_compositor.h"
#include "khoros/core/topology.h"
#include "khoros/uring/pbuf.h"
#include "khoros/uring/ring.h"
#include "khoros/wayland/client.h"
#include "khoros/wayland/xdg.h"
#include <sys/socket.h>
#include <unistd.h>
#include <string.h>

/*
 * Full XDG shell lifecycle against the mock compositor, driven through the
 * production paths: registry roundtrip, batched bind, batched toplevel
 * creation, multishot PBUF inbound, pump classification, and the
 * ping->pong / configure->ack state machine.
 */

/* Resolve one harvested evt to its wire payload (tier-0 multishot only). */
static bool khr_test_xdg_view(khr_topology_t* topo, const khr_cqe_event_t* evt,
                              const uint8_t** out_data, size_t* out_len,
                              khr_recvmsg_view_t* out_view) {
    if (evt->res <= 0) {
        return false;
    }
    uint16_t bid = (uint16_t)(evt->flags >> IORING_CQE_BUFFER_SHIFT);
    uint8_t* raw = khr_pbuf_data(&topo->pbuf_tier0, bid);
    if (raw == nullptr) {
        return false;
    }
    if (!khr_recvmsg_parse(raw, topo->pbuf_tier0.buf_size, 0, 0, out_view)) {
        return false;
    }
    *out_data = out_view->payload;
    *out_len = out_view->payload_len;
    return true;
}

/* Drain every parked/multishot event through khr_xdg_consume. */
static uint32_t khr_test_xdg_drain(khr_topology_t* topo, khr_wl_client_t* client,
                                   khr_xdg_shell_t* shell) {
    uint32_t handled = 0;
    for (;;) {
        khr_cqe_event_t evt = {};
        if (!khr_topology_pop_cqe(topo, &evt)) {
            break;
        }
        const uint8_t* data = nullptr;
        size_t len = 0;
        khr_recvmsg_view_t view = {};
        if (khr_test_xdg_view(topo, &evt, &data, &len, &view) && data != nullptr) {
            handled += khr_xdg_consume(client, shell, data, len);
            (void)khr_xdg_ack_pending(client, shell);
        }
        khr_topology_recycle_cqe_buffer(topo, &evt);
    }
    return handled;
}

[[nodiscard]]
static bool test_xdg_shell_lifecycle_body(khr_topology_t* topo_ctx) {
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
    khr_wl_client_attach_pbufs(&client, &topo.pbuf_tier0, nullptr);
    khr_wl_client_attach_topology(&client, &topo);

    /* 1. Registry discovery. */
    TEST_ASSERT(mock_compositor_send_globals(&comp), "send globals failed");
    TEST_ASSERT(mock_compositor_send_sync_done(&comp, KHR_WL_CALLBACK_ID, 1),
                "send sync done failed");
    TEST_ASSERT(khr_wl_client_roundtrip(&client), "registry roundtrip failed");
    TEST_ASSERT_NOT_NULL(khr_wl_client_find_global(&client, "wl_compositor"),
                         "wl_compositor missing");
    TEST_ASSERT_NOT_NULL(khr_wl_client_find_global(&client, "xdg_wm_base"),
                         "xdg_wm_base missing");

    /* 2. Bind both globals in one batched send. */
    khr_xdg_shell_t shell = {};
    khr_xdg_init(&shell);
    TEST_ASSERT(khr_xdg_bind(&client, &shell), "xdg bind failed");
    TEST_ASSERT_EQ(shell.state, (uint32_t)KHR_XDG_BOUND, "state must be BOUND");
    TEST_ASSERT(mock_compositor_drain(&comp) >= 2, "bind requests missing");
    TEST_ASSERT_EQ(comp.client_compositor_id, shell.compositor_id, "compositor id mismatch");
    TEST_ASSERT_EQ(comp.client_wm_base_id, shell.wm_base_id, "wm_base id mismatch");

    /* 3. Create the toplevel: surface + xdg_surface + toplevel + title/app_id
     * plus the empty first commit. Attach gate must still be closed. */
    TEST_ASSERT(khr_xdg_create_toplevel(&client, &shell, "Khoros", "khoros-engine"),
                "create toplevel failed");
    TEST_ASSERT_EQ(shell.state, (uint32_t)KHR_XDG_TOPLEVEL, "state must be TOPLEVEL");
    TEST_ASSERT(!khr_xdg_can_attach(&shell), "attach gate must be closed pre-configure");
    TEST_ASSERT(mock_compositor_drain(&comp) >= 5, "toplevel requests missing");
    TEST_ASSERT_EQ(comp.surface_id, shell.surface_id, "surface id mismatch");
    TEST_ASSERT_EQ(comp.xdg_surface_id, shell.xdg_surface_id, "xdg_surface id mismatch");
    TEST_ASSERT_EQ(comp.xdg_toplevel_id, shell.xdg_toplevel_id, "toplevel id mismatch");
    TEST_ASSERT(comp.commit_count >= 1, "empty first commit missing");

    /* 4. Steady state goes through multishot PBUF + pump, like production. */
    TEST_ASSERT(khr_wl_client_arm_inbound(&client), "arm inbound failed");
    TEST_ASSERT(mock_compositor_send_ping(&comp, shell.wm_base_id, 99), "send ping failed");
    TEST_ASSERT(mock_compositor_send_xdg_configure(&comp, shell.xdg_toplevel_id,
                                                   shell.xdg_surface_id,
                                                   1280, 720, 7),
                "send configure failed");
    TEST_ASSERT_EQ(khr_test_xdg_drain(&topo, &client, &shell), 3U,
                   "must handle ping + 2 configure messages");

    /* Ping answered immediately with the same serial. */
    TEST_ASSERT_EQ(shell.ping_count, 1U, "ping not observed");
    TEST_ASSERT_EQ(shell.last_ping_serial, 99U, "ping serial mismatch");
    TEST_ASSERT(mock_compositor_drain(&comp) >= 2, "pong/ack requests missing");
    TEST_ASSERT_EQ(comp.pong_count, 1U, "pong not sent");
    TEST_ASSERT_EQ(comp.last_pong_serial, 99U, "pong serial mismatch");

    /* Configure acknowledged; geometry stored; attach gate opens. */
    TEST_ASSERT(shell.configured, "shell must be configured");
    TEST_ASSERT_EQ(shell.state, (uint32_t)KHR_XDG_CONFIGURED, "state must be CONFIGURED");
    TEST_ASSERT_EQ(comp.ack_configure_count, 1U, "ack_configure not sent");
    TEST_ASSERT_EQ(comp.last_ack_serial, 7U, "ack serial mismatch");
    TEST_ASSERT_EQ(shell.width, 1280, "configure width mismatch");
    TEST_ASSERT_EQ(shell.height, 720, "configure height mismatch");
    TEST_ASSERT(khr_xdg_can_attach(&shell), "attach gate must be open post-configure");

    /* 5. Close event shuts the gate. */
    khr_wl_msg_buf_t close_msg = {};
    khr_wl_buf_init(&close_msg);
    TEST_ASSERT(khr_wl_encode_header(&close_msg, shell.xdg_toplevel_id,
                                     KHR_XDG_TOPLEVEL_EVENT_CLOSE, 8),
                "encode close failed");
    TEST_ASSERT(mock_compositor_send_raw(&comp, close_msg.data, close_msg.size),
                "send close failed");
    TEST_ASSERT_EQ(khr_test_xdg_drain(&topo, &client, &shell), 1U, "close not handled");
    TEST_ASSERT(shell.closed, "closed flag missing");
    TEST_ASSERT(!khr_xdg_can_attach(&shell), "attach gate must close on close event");

    TEST_ASSERT_EQ(khr_topology_staged_count(&topo), (size_t)0, "evt_q must drain to 0");

    mock_compositor_destroy(&comp);
#undef topo
    return true;
}

[[nodiscard]]
bool test_xdg_shell_lifecycle(void) {
    khr_topology_t topo = {};
    if (!khr_topology_init(&topo)) {
        return false;
    }
    bool ok = test_xdg_shell_lifecycle_body(&topo);
    khr_topology_destroy(&topo);
    return ok;
}

#include "test_framework.h"
#include "khoros/uring/ring.h"
#include "khoros/wayland/wire.h"
#include "khoros/wayland/client.h"
#include "khoros/core/cpu.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>

[[nodiscard]]
bool test_wayland_wire_encoding_decoding(void) {
    khr_wl_msg_buf_t buf = {};
    khr_wl_buf_init(&buf);

    /* Encode a synthetic header: obj=42, op=3, size=24 */
    TEST_ASSERT(khr_wl_encode_header(&buf, 42, 3, 24), "Header encode failed");
    TEST_ASSERT(khr_wl_encode_u32(&buf, 1337), "u32 encode failed");
    TEST_ASSERT(khr_wl_encode_string(&buf, "wl_surface"), "string encode failed");

    /* Decode header */
    khr_wl_msg_header_t hdr = {};
    TEST_ASSERT(khr_wl_decode_header(buf.data, buf.size, &hdr), "Header decode failed");
    TEST_ASSERT(hdr.object_id == 42, "Object ID mismatch");
    TEST_ASSERT(hdr.opcode == 3, "Opcode mismatch");
    TEST_ASSERT(hdr.size == 24, "Size mismatch");

    /* Decode payload */
    size_t offset = 8;
    uint32_t val = 0;
    TEST_ASSERT(khr_wl_decode_u32(buf.data, buf.size, &offset, &val), "u32 decode failed");
    TEST_ASSERT(val == 1337, "Decoded value mismatch");

    const char* str = nullptr;
    uint32_t str_len = 0;
    TEST_ASSERT(khr_wl_decode_string(buf.data, buf.size, &offset, &str, &str_len), "String decode failed");
    TEST_ASSERT(strcmp(str, "wl_surface") == 0, "Decoded string content mismatch");
    TEST_ASSERT(str_len == 11, "Decoded string length mismatch (10 chars + NUL)");

    return true;
}

[[nodiscard]]
bool test_wayland_raw_uring_roundtrip(void) {
    /* Initialize Ring A (Real-Time Ring) */
    khr_uring_config_t cfg = {
        .sq_entries = 64,
        .cq_entries = 256,
        .flags = IORING_SETUP_SINGLE_ISSUER |
                 IORING_SETUP_DEFER_TASKRUN |
                 IORING_SETUP_COOP_TASKRUN  |
                 IORING_SETUP_NO_SQARRAY    |
                 IORING_SETUP_CLAMP         |
                 IORING_SETUP_CQSIZE,
        .register_ring_fd = true,
    };

    khr_uring_t ring = {};
    TEST_ASSERT(khr_uring_init(&ring, &cfg), "Failed to initialize raw io_uring Ring A");

    /* Connect Wayland Client over raw io_uring */
    khr_wl_client_t client = {};
    bool connected = khr_wl_client_connect(&client, &ring, nullptr);
    if (!connected) {
        printf("  (Notice: Wayland compositor not accessible, skipping live socket test) ");
        khr_uring_destroy(&ring);
        return true;
    }

    /* Perform live roundtrip to query compositor globals */
    bool roundtrip_ok = khr_wl_client_roundtrip(&client);
    TEST_ASSERT(roundtrip_ok, "Wayland client roundtrip over raw io_uring failed");
    TEST_ASSERT(client.globals_count > 0, "Expected at least 1 global advertised by compositor");
    TEST_ASSERT(client.compositor_name > 0, "wl_compositor must be advertised");

    /* Verify core protocol globals exist */
    auto comp = khr_wl_client_find_global(&client, "wl_compositor");
    TEST_ASSERT(comp != nullptr, "wl_compositor must be in globals registry");
    TEST_ASSERT(comp->version >= 1, "wl_compositor version must be >= 1");

    khr_wl_client_disconnect(&client);
    khr_uring_destroy(&ring);
    return true;
}

[[nodiscard]]
bool test_wayland_wire_fuzz_and_edge_cases(void) {
    /* 1. Header decode on undersized buffer */
    uint8_t short_data[7] = { 0 };
    khr_wl_msg_header_t hdr = {};
    TEST_ASSERT(!khr_wl_decode_header(short_data, sizeof(short_data), &hdr), "short header must fail");
    TEST_ASSERT(!khr_wl_decode_header(nullptr, 8, &hdr), "null data must fail");
    TEST_ASSERT(!khr_wl_decode_header(short_data, 8, nullptr), "null out_hdr must fail");

    /* 2. Buffer encode overflow protection */
    khr_wl_msg_buf_t buf = {};
    khr_wl_buf_init(&buf);
    buf.size = sizeof(buf.data) - 4;
    TEST_ASSERT(!khr_wl_encode_header(&buf, 1, 1, 8), "header encode over capacity must fail");
    TEST_ASSERT(!khr_wl_encode_string(&buf, "toolong"), "string encode over capacity must fail");

    /* 3. String padding checks across lengths 0 through 8 */
    for (uint32_t len = 0; len <= 8; len++) {
        uint32_t padded = khr_wl_pad4(len);
        TEST_ASSERT((padded % 4) == 0, "pad4 must align to 4 bytes");
        TEST_ASSERT(padded >= len, "pad4 must be >= len");
        TEST_ASSERT(padded < len + 4, "pad4 must not overshoot by 4 or more");
    }

    /* 4. Truncated string decode */
    khr_wl_buf_init(&buf);
    TEST_ASSERT(khr_wl_encode_header(&buf, 1, 1, 16), "header encode failed");
    TEST_ASSERT(khr_wl_encode_string(&buf, "testing"), "string encode failed");

    size_t offset = 8;
    const char* str = nullptr;
    uint32_t slen = 0;
    TEST_ASSERT(!khr_wl_decode_string(buf.data, 12, &offset, &str, &slen), "truncated string must fail");

    /* 5. Extreme integer values */
    khr_wl_buf_init(&buf);
    TEST_ASSERT(khr_wl_encode_u32(&buf, UINT32_MAX), "UINT32_MAX encode failed");
    TEST_ASSERT(khr_wl_encode_i32(&buf, INT32_MIN), "INT32_MIN encode failed");
    TEST_ASSERT(khr_wl_encode_i32(&buf, INT32_MAX), "INT32_MAX encode failed");

    offset = 0;
    uint32_t u_got = 0;
    TEST_ASSERT(khr_wl_decode_u32(buf.data, buf.size, &offset, &u_got), "u32 decode failed");
    TEST_ASSERT_EQ(u_got, UINT32_MAX, "UINT32_MAX mismatch");

    int32_t i_got = 0;
    TEST_ASSERT(khr_wl_decode_i32(buf.data, buf.size, &offset, &i_got), "i32 decode failed");
    TEST_ASSERT_EQ(i_got, INT32_MIN, "INT32_MIN mismatch");

    TEST_ASSERT(khr_wl_decode_i32(buf.data, buf.size, &offset, &i_got), "i32 decode failed");
    TEST_ASSERT_EQ(i_got, INT32_MAX, "INT32_MAX mismatch");

    return true;
}

[[nodiscard]]
bool test_wayland_wire_deep_fuzz_and_security(void) {
    /* 1. Header with size < 8 must be rejected as invalid wire header */
    uint8_t small_hdr[8] = {};
    uint64_t small_packed = (uint64_t)1 | ((uint64_t)(((uint32_t)4 << 16) | 0) << 32);
    memcpy(small_hdr, &small_packed, 8);
    khr_wl_msg_header_t hdr = {};
    TEST_ASSERT(!khr_wl_decode_header(small_hdr, sizeof(small_hdr), &hdr), "header with size < 8 must be rejected");

    /* 2. Header with unaligned size (e.g. 9 bytes) must be rejected */
    uint64_t unaligned_packed = (uint64_t)1 | ((uint64_t)(((uint32_t)9 << 16) | 0) << 32);
    memcpy(small_hdr, &unaligned_packed, 8);
    TEST_ASSERT(!khr_wl_decode_header(small_hdr, sizeof(small_hdr), &hdr), "header with unaligned size must be rejected");

    /* 3. Non-NUL-terminated string attack: buffer with raw_len = 4, but no '\0' byte */
    uint8_t malformed_str[16] = {};
    uint32_t fake_len = 4;
    memcpy(malformed_str, &fake_len, 4);
    malformed_str[4] = 'W';
    malformed_str[5] = 'A';
    malformed_str[6] = 'Y';
    malformed_str[7] = 'L'; /* NOT NUL-terminated */
    size_t off = 0;
    const char* decoded_str = nullptr;
    uint32_t decoded_len = 0;
    TEST_ASSERT(!khr_wl_decode_string(malformed_str, sizeof(malformed_str), &off, &decoded_str, &decoded_len),
                "non-NUL terminated string must be rejected by decoder");

    /* 4. Integer overflow attempt in string length (UINT32_MAX - 1) */
    uint32_t huge_len = UINT32_MAX - 1;
    memcpy(malformed_str, &huge_len, 4);
    off = 0;
    TEST_ASSERT(!khr_wl_decode_string(malformed_str, sizeof(malformed_str), &off, &decoded_str, &decoded_len),
                "integer-overflowing string length must be rejected");

    /* 5. Valid 0-length nullable string */
    uint32_t zero_len = 0;
    memcpy(malformed_str, &zero_len, 4);
    off = 0;
    TEST_ASSERT(khr_wl_decode_string(malformed_str, sizeof(malformed_str), &off, &decoded_str, &decoded_len),
                "0-length string must decode cleanly as empty string");
    TEST_ASSERT_NOT_NULL(decoded_str, "decoded pointer must not be null");
    TEST_ASSERT_EQ(decoded_len, 0U, "decoded length must be 0");
    TEST_ASSERT_EQ(decoded_str[0], '\0', "decoded string must be empty");

    /* 6. Empty string encode/decode roundtrip */
    khr_wl_msg_buf_t buf = {};
    khr_wl_buf_init(&buf);
    TEST_ASSERT(khr_wl_encode_string(&buf, ""), "empty string encode failed");
    off = 0;
    TEST_ASSERT(khr_wl_decode_string(buf.data, buf.size, &off, &decoded_str, &decoded_len),
                "empty string decode failed");
    TEST_ASSERT_EQ(decoded_len, 1U, "encoded empty string has 1 byte (NUL)");
    TEST_ASSERT_EQ(decoded_str[0], '\0', "decoded empty string content must be NUL");

    return true;
}

typedef struct {
    int fd;
    _Atomic bool ready;
    _Atomic bool done;
} mock_compositor_ctx_t;

static void* mock_compositor_worker(void* arg) {
    mock_compositor_ctx_t* ctx = (mock_compositor_ctx_t*)arg;

    /* Step 1: Wait for client requests (get_registry = 12 bytes + sync = 12 bytes) */
    uint8_t req_buf[64] = {};
    ssize_t nread = 0;
    while (nread < 24) {
        ssize_t r = recv(ctx->fd, req_buf + nread, sizeof(req_buf) - (size_t)nread, 0);
        if (r <= 0) {
            return nullptr;
        }
        nread += r;
    }

    /* Step 2: Build responses: 5 core globals + callback.done */
    khr_wl_msg_buf_t resp = {};
    khr_wl_buf_init(&resp);

    /* Global 1: wl_compositor (name=1, ver=4) */
    uint32_t s_len = (uint32_t)strlen("wl_compositor") + 1;
    uint32_t s_pad = khr_wl_pad4(s_len);
    (void)khr_wl_encode_header(&resp, KHR_WL_REGISTRY_ID, KHR_WL_REGISTRY_EVENT_GLOBAL, (uint16_t)(8 + 4 + 4 + s_pad + 4));
    (void)khr_wl_encode_u32(&resp, 1);
    (void)khr_wl_encode_string(&resp, "wl_compositor");
    (void)khr_wl_encode_u32(&resp, 4);

    /* Global 2: xdg_wm_base (name=2, ver=3) */
    s_len = (uint32_t)strlen("xdg_wm_base") + 1;
    s_pad = khr_wl_pad4(s_len);
    (void)khr_wl_encode_header(&resp, KHR_WL_REGISTRY_ID, KHR_WL_REGISTRY_EVENT_GLOBAL, (uint16_t)(8 + 4 + 4 + s_pad + 4));
    (void)khr_wl_encode_u32(&resp, 2);
    (void)khr_wl_encode_string(&resp, "xdg_wm_base");
    (void)khr_wl_encode_u32(&resp, 3);

    /* Global 3: wl_shm (name=3, ver=1) */
    s_len = (uint32_t)strlen("wl_shm") + 1;
    s_pad = khr_wl_pad4(s_len);
    (void)khr_wl_encode_header(&resp, KHR_WL_REGISTRY_ID, KHR_WL_REGISTRY_EVENT_GLOBAL, (uint16_t)(8 + 4 + 4 + s_pad + 4));
    (void)khr_wl_encode_u32(&resp, 3);
    (void)khr_wl_encode_string(&resp, "wl_shm");
    (void)khr_wl_encode_u32(&resp, 1);

    /* Global 4: wl_seat (name=4, ver=7) */
    s_len = (uint32_t)strlen("wl_seat") + 1;
    s_pad = khr_wl_pad4(s_len);
    (void)khr_wl_encode_header(&resp, KHR_WL_REGISTRY_ID, KHR_WL_REGISTRY_EVENT_GLOBAL, (uint16_t)(8 + 4 + 4 + s_pad + 4));
    (void)khr_wl_encode_u32(&resp, 4);
    (void)khr_wl_encode_string(&resp, "wl_seat");
    (void)khr_wl_encode_u32(&resp, 7);

    /* Global 5: wp_linux_drm_syncobj_manager_v1 (name=5, ver=1) */
    s_len = (uint32_t)strlen("wp_linux_drm_syncobj_manager_v1") + 1;
    s_pad = khr_wl_pad4(s_len);
    (void)khr_wl_encode_header(&resp, KHR_WL_REGISTRY_ID, KHR_WL_REGISTRY_EVENT_GLOBAL, (uint16_t)(8 + 4 + 4 + s_pad + 4));
    (void)khr_wl_encode_u32(&resp, 5);
    (void)khr_wl_encode_string(&resp, "wp_linux_drm_syncobj_manager_v1");
    (void)khr_wl_encode_u32(&resp, 1);

    /* Event: wl_callback.done(serial=12345) */
    (void)khr_wl_encode_header(&resp, KHR_WL_CALLBACK_ID, KHR_WL_CALLBACK_EVENT_DONE, 12);
    (void)khr_wl_encode_u32(&resp, 12345);

    /* Step 3: Transmit all responses back to client */
    (void)send(ctx->fd, resp.data, resp.size, 0);
    atomic_store_explicit(&ctx->done, true, memory_order_release);
    return nullptr;
}

[[nodiscard]]
bool test_wayland_client_mock_roundtrip(void) {
    int sv[2] = { -1, -1 };
    TEST_ASSERT(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0, "socketpair failed");

    khr_uring_config_t cfg = khr_uring_config_ring_a();
    khr_uring_t ring = {};
    TEST_ASSERT(khr_uring_init(&ring, &cfg), "Ring A init failed");

    khr_wl_client_t client = {
        .sock_fd = sv[0],
        .ring = &ring,
    };

    mock_compositor_ctx_t ctx = {
        .fd = sv[1],
        .ready = false,
        .done = false,
    };

    pthread_t th = 0;
    TEST_ASSERT(pthread_create(&th, nullptr, mock_compositor_worker, &ctx) == 0, "mock thread failed");

    /* Execute real client roundtrip over raw io_uring */
    bool ok = khr_wl_client_roundtrip(&client);
    (void)pthread_join(th, nullptr);

    TEST_ASSERT(ok, "mock roundtrip over raw io_uring failed");
    TEST_ASSERT(client.sync_completed, "sync_completed must be true");
    TEST_ASSERT_EQ(client.globals_count, 5U, "expected exactly 5 globals discovered");
    TEST_ASSERT_EQ(client.compositor_name, 1U, "compositor name mismatch");
    TEST_ASSERT_EQ(client.compositor_version, 4U, "compositor version mismatch");
    TEST_ASSERT_EQ(client.xdg_wm_base_name, 2U, "xdg_wm_base name mismatch");
    TEST_ASSERT_EQ(client.xdg_wm_base_version, 3U, "xdg_wm_base version mismatch");
    TEST_ASSERT_EQ(client.shm_name, 3U, "wl_shm name mismatch");
    TEST_ASSERT_EQ(client.seat_name, 4U, "wl_seat name mismatch");
    TEST_ASSERT_EQ(client.seat_version, 7U, "wl_seat version mismatch");
    TEST_ASSERT_EQ(client.syncobj_manager_name, 5U, "syncobj_manager name mismatch");

    auto comp = khr_wl_client_find_global(&client, "wl_compositor");
    TEST_ASSERT_NOT_NULL(comp, "find_global wl_compositor failed");
    TEST_ASSERT_EQ(comp->version, 4U, "find_global version mismatch");

    close(sv[0]);
    close(sv[1]);
    khr_uring_destroy(&ring);
    return true;
}

typedef struct {
    int listen_fd;
    _Atomic bool listening;
} mock_direct_server_ctx_t;

static void* mock_direct_server_worker(void* arg) {
    mock_direct_server_ctx_t* ctx = (mock_direct_server_ctx_t*)arg;
    atomic_store_explicit(&ctx->listening, true, memory_order_release);
    int cfd = accept(ctx->listen_fd, nullptr, nullptr);
    if (cfd < 0) {
        return nullptr;
    }
    mock_compositor_ctx_t cctx = {
        .fd = cfd,
        .ready = false,
        .done = false,
    };
    (void)mock_compositor_worker(&cctx);
    close(cfd);
    return nullptr;
}

[[nodiscard]]
bool test_wayland_direct_socket_connect_and_roundtrip(void) {
    char sock_path[108] = "/tmp/khr_wl_direct_XXXXXX";
    int tfd = mkstemp(sock_path);
    TEST_ASSERT(tfd >= 0, "mkstemp for mock direct socket failed");
    close(tfd);
    unlink(sock_path);

    int lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    TEST_ASSERT(lfd >= 0, "socket for server failed");

    struct sockaddr_un sun = { .sun_family = AF_UNIX };
    snprintf(sun.sun_path, sizeof(sun.sun_path), "%s", sock_path);
    TEST_ASSERT(bind(lfd, (struct sockaddr*)&sun, sizeof(sun)) == 0, "bind server socket failed");
    TEST_ASSERT(listen(lfd, 1) == 0, "listen server socket failed");

    mock_direct_server_ctx_t sctx = {
        .listen_fd = lfd,
        .listening = false,
    };
    pthread_t th = 0;
    TEST_ASSERT(pthread_create(&th, nullptr, mock_direct_server_worker, &sctx) == 0, "server thread failed");
    while (!atomic_load_explicit(&sctx.listening, memory_order_acquire)) {
        khr_cpu_pause();
    }

    khr_uring_config_t cfg = khr_uring_config_ring_a();
    khr_uring_t ring = {};
    TEST_ASSERT(khr_uring_init(&ring, &cfg), "Ring A init failed");
    TEST_ASSERT(ring.files_registered, "Ring A must have registered sparse files");

    khr_wl_client_t client = {};
    bool connected = khr_wl_client_connect(&client, &ring, sock_path);
    TEST_ASSERT(connected, "Direct descriptor connect failed");
    TEST_ASSERT(client.is_direct, "Client must be using direct descriptors");
    TEST_ASSERT_EQ(client.sock_fd, (int)KHR_DIRECT_SLOT_WAYLAND, "Direct descriptor slot mismatch");

    bool roundtrip_ok = khr_wl_client_roundtrip(&client);
    (void)pthread_join(th, nullptr);
    close(lfd);
    unlink(sock_path);

    TEST_ASSERT(roundtrip_ok, "Direct descriptor Wayland roundtrip failed");
    TEST_ASSERT(client.sync_completed, "sync_completed must be true");
    TEST_ASSERT_EQ(client.globals_count, 5U, "expected 5 globals");
    TEST_ASSERT_EQ(client.compositor_name, 1U, "compositor name mismatch");
    TEST_ASSERT_EQ(client.compositor_version, 4U, "compositor version mismatch");

    auto comp = khr_wl_client_find_global(&client, "wl_compositor");
    TEST_ASSERT_NOT_NULL(comp, "find_global wl_compositor failed");
    TEST_ASSERT_EQ(comp->version, 4U, "compositor version mismatch");

    khr_wl_client_disconnect(&client);
    TEST_ASSERT_EQ(client.sock_fd, -1, "Client sock_fd must be reset to -1 after disconnect");
    TEST_ASSERT(!client.is_direct, "Client is_direct must be false after disconnect");

    khr_uring_destroy(&ring);
    return true;
}

typedef struct {
    int listen_fd;
    _Atomic bool ready;
    _Atomic bool done;
    _Atomic bool success;
} mock_pure_server_ctx_t;

static void* mock_pure_server_worker(void* arg) {
    mock_pure_server_ctx_t* ctx = (mock_pure_server_ctx_t*)arg;
    khr_uring_config_t cfg = khr_uring_config_ring_a();
    khr_uring_t sring = {};
    if (!khr_uring_init(&sring, &cfg)) {
        return nullptr;
    }

    /* Arm direct accept into direct slot 2 */
    struct io_uring_sqe* asqe = khr_uring_prep_accept(
        &sring, ctx->listen_fd, nullptr, nullptr, 0, 2, true, KHR_TAG_ACCEPT);
    if (asqe == nullptr) {
        khr_uring_destroy(&sring);
        return nullptr;
    }
    if (khr_uring_submit(&sring, 0) < 0) {
        khr_uring_destroy(&sring);
        return nullptr;
    }

    atomic_store_explicit(&ctx->ready, true, memory_order_release);

    /* Wait for accept completion */
    struct io_uring_cqe* cqe = nullptr;
    if (!khr_uring_wait_cqe_timeout(&sring, &cqe, 2'000) || cqe == nullptr || cqe->res < 0) {
        khr_uring_destroy(&sring);
        return nullptr;
    }
    khr_uring_cqe_seen(&sring, cqe);

    /* Receive 16-byte message on direct slot 2 */
    char s_buf[32] = {};
    struct io_uring_sqe* rsqe = khr_uring_prep_recv(&sring, 2, s_buf, sizeof(s_buf), 0, true, 301);
    if (rsqe == nullptr) {
        khr_uring_destroy(&sring);
        return nullptr;
    }
    if (khr_uring_submit(&sring, 1) < 0) {
        khr_uring_destroy(&sring);
        return nullptr;
    }
    if (!khr_uring_wait_cqe_timeout(&sring, &cqe, 2'000) || cqe == nullptr || cqe->res <= 0) {
        khr_uring_destroy(&sring);
        return nullptr;
    }
    int nread = cqe->res;
    khr_uring_cqe_seen(&sring, cqe);

    if (nread == 16 && memcmp(s_buf, "pure-uring-sock", 15) == 0) {
        atomic_store_explicit(&ctx->success, true, memory_order_release);
    }

    /* Close direct slot 2 */
    struct io_uring_sqe* csqe = khr_uring_prep_close(&sring, 2, true, 302);
    if (csqe != nullptr) {
        (void)khr_uring_submit(&sring, 1);
        if (khr_uring_wait_cqe_timeout(&sring, &cqe, 1'000) && cqe != nullptr) {
            khr_uring_cqe_seen(&sring, cqe);
        }
    }

    khr_uring_destroy(&sring);
    atomic_store_explicit(&ctx->done, true, memory_order_release);
    return nullptr;
}

[[nodiscard]]
bool test_wayland_pure_ring_native_socket_and_connect(void) {
    /* 1. Failure resilience: Attempt connect to non-existent path */
    khr_uring_config_t ccfg = khr_uring_config_ring_a();
    khr_uring_t cring = {};
    TEST_ASSERT(khr_uring_init(&cring, &ccfg), "Client ring init failed");

    khr_wl_client_t client_fail = {};
    bool fail_ok = khr_wl_client_connect(&client_fail, &cring, "/tmp/khr_nonexistent_socket_path_12345.sock");
    TEST_ASSERT(!fail_ok, "connect to non-existent socket must fail");
    TEST_ASSERT_EQ(client_fail.sock_fd, -1, "failed client sock_fd must be -1");
    TEST_ASSERT(!client_fail.is_direct, "failed client is_direct must be false");

    /* 2. Pure io_uring End-to-End Direct Socket Lifecycle */
    char sock_path[108] = "/tmp/khr_pure_sock_XXXXXX";
    int tfd = mkstemp(sock_path);
    TEST_ASSERT(tfd >= 0, "mkstemp failed");
    close(tfd);
    unlink(sock_path);

    int lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    TEST_ASSERT(lfd >= 0, "server socket creation failed");
    struct sockaddr_un sun = { .sun_family = AF_UNIX };
    snprintf(sun.sun_path, sizeof(sun.sun_path), "%s", sock_path);
    TEST_ASSERT(bind(lfd, (struct sockaddr*)&sun, sizeof(sun)) == 0, "server bind failed");
    TEST_ASSERT(listen(lfd, 1) == 0, "server listen failed");

    mock_pure_server_ctx_t sctx = {};
    sctx.listen_fd = lfd;
    pthread_t sth = 0;
    TEST_ASSERT(pthread_create(&sth, nullptr, mock_pure_server_worker, &sctx) == 0, "server thread failed");

    while (!atomic_load_explicit(&sctx.ready, memory_order_acquire)) {
        khr_cpu_pause();
    }

    khr_wl_client_t client = {};
    bool conn_ok = khr_wl_client_connect(&client, &cring, sock_path);
    TEST_ASSERT(conn_ok, "pure io_uring client connect failed");
    TEST_ASSERT(client.is_direct, "client must be direct");
    TEST_ASSERT_EQ(client.sock_fd, (int)KHR_DIRECT_SLOT_WAYLAND, "client direct slot mismatch");

    /* Send 16-byte message over direct slot */
    const char msg[16] = "pure-uring-sock";
    struct io_uring_sqe* ssqe = khr_uring_prep_send(
        &cring, client.sock_fd, msg, sizeof(msg), 0, true, 201);
    TEST_ASSERT_NOT_NULL(ssqe, "prep_send direct failed");
    TEST_ASSERT(khr_uring_submit(&cring, 1) >= 0, "submit send failed");

    struct io_uring_cqe* cqe = nullptr;
    TEST_ASSERT(khr_uring_wait_cqe_timeout(&cring, &cqe, 2'000), "wait send cqe failed");
    TEST_ASSERT_NOT_NULL(cqe, "send cqe must not be null");
    TEST_ASSERT_EQ(cqe->res, (int)sizeof(msg), "send bytes mismatch");
    khr_uring_cqe_seen(&cring, cqe);

    (void)pthread_join(sth, nullptr);
    close(lfd);
    unlink(sock_path);

    TEST_ASSERT(atomic_load_explicit(&sctx.success, memory_order_acquire), "server direct recv data mismatch");

    /* Disconnect client via direct close */
    khr_wl_client_disconnect(&client);
    TEST_ASSERT_EQ(client.sock_fd, -1, "client sock_fd must be -1 after disconnect");
    TEST_ASSERT(!client.is_direct, "client is_direct must be false after disconnect");

    khr_uring_destroy(&cring);
    return true;
}

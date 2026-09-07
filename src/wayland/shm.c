#include "khoros/wayland/shm.h"

#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

[[nodiscard]]
bool khr_shm_bind(khr_wl_client_t* client, uint32_t* out_shm_id) {
    if (client == nullptr || out_shm_id == nullptr) {
        return false;
    }
    const khr_wl_global_t* shm = khr_wl_client_find_global(client, "wl_shm");
    if (shm == nullptr) {
        return false;
    }
    uint32_t id = khr_wl_client_alloc_id(client);
    if (id == 0) {
        return false;
    }
    khr_wl_msg_buf_t out = {};
    khr_wl_buf_init(&out);
    const char* iface = "wl_shm";
    uint32_t slen = (uint32_t)strlen(iface) + 1U;
    uint16_t total = (uint16_t)(24U + khr_wl_pad4(slen));
    if (!khr_wl_encode_header(&out, KHR_WL_REGISTRY_ID, KHR_WL_REGISTRY_BIND, total) ||
        !khr_wl_encode_u32(&out, shm->name) ||
        !khr_wl_encode_string(&out, iface) ||
        !khr_wl_encode_u32(&out, shm->version) ||
        !khr_wl_encode_u32(&out, id) ||
        !khr_wl_client_send_skip(client, out.data, out.size)) {
        return false;
    }
    *out_shm_id = id;
    return true;
}

[[nodiscard]]
bool khr_shm_pool_init(khr_wl_client_t* client, uint32_t shm_id, size_t size,
                       khr_shm_pool_t* out_pool) {
    if (client == nullptr || shm_id == 0 || size == 0 || out_pool == nullptr) {
        return false;
    }
    *out_pool = (khr_shm_pool_t){ .fd = -1 };
    uint32_t pool_id = khr_wl_client_alloc_id(client);
    if (pool_id == 0) {
        return false;
    }
    /* Page-align: some compositors reject create_buffer when the pool is a
     * partial page (cursor 24×24×4 = 2304 died live on Mutter). */
    constexpr size_t page = 4'096;
    size_t pooled = (size + page - 1U) & ~(page - 1U);
    if (pooled < page) {
        pooled = page;
    }
    int fd = memfd_create("khoros-shm-pool", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) {
        return false;
    }
    /* Size FIRST: F_SEAL_GROW forbids growth, so sealing precedes nothing.
     * Seals stop silent SIGBUS truncation races; best-effort on odd kernels. */
    if (ftruncate(fd, (off_t)pooled) != 0) {
        close(fd);
        return false;
    }
    (void)fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW);
    void* addr = mmap(nullptr, pooled, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        close(fd);
        return false;
    }
    khr_wl_msg_buf_t out = {};
    khr_wl_buf_init(&out);
    bool enc = khr_wl_encode_header(&out, shm_id, KHR_WL_SHM_CREATE_POOL, 16) &&
               khr_wl_encode_u32(&out, pool_id) &&
               khr_wl_encode_i32(&out, (int32_t)pooled);
    if (!enc) {
        munmap(addr, pooled);
        close(fd);
        return false;
    }
    if (!khr_wl_client_send_with_fd(client, out.data, out.size, fd)) {
        munmap(addr, pooled);
        close(fd);
        return false;
    }
    *out_pool = (khr_shm_pool_t){
        .pool_id = pool_id,
        .fd = fd,
        .size = pooled,
        .addr = addr,
    };
    return true;
}

[[nodiscard]]
bool khr_shm_buffer_create(khr_wl_client_t* client, const khr_shm_pool_t* pool,
                           uint32_t offset, uint32_t w, uint32_t h,
                           uint32_t stride, uint32_t* out_buffer_id) {
    if (client == nullptr || pool == nullptr || pool->pool_id == 0 ||
        w == 0 || h == 0 || out_buffer_id == nullptr) {
        return false;
    }
    if ((size_t)offset + (size_t)stride * h > pool->size) {
        return false;
    }
    uint32_t id = khr_wl_client_alloc_id(client);
    if (id == 0) {
        return false;
    }
    khr_wl_msg_buf_t out = {};
    khr_wl_buf_init(&out);
    if (!khr_wl_encode_header(&out, pool->pool_id, KHR_WL_SHM_POOL_CREATE_BUFFER, 32) ||
        !khr_wl_encode_u32(&out, id) ||
        !khr_wl_encode_i32(&out, (int32_t)offset) ||
        !khr_wl_encode_i32(&out, (int32_t)w) ||
        !khr_wl_encode_i32(&out, (int32_t)h) ||
        !khr_wl_encode_i32(&out, (int32_t)stride) ||
        !khr_wl_encode_u32(&out, KHR_WL_SHM_FORMAT_ARGB8888) ||
        !khr_wl_client_send_skip(client, out.data, out.size)) {
        return false;
    }
    *out_buffer_id = id;
    return true;
}

[[nodiscard]]
bool khr_shm_attach_commit(khr_wl_client_t* client, uint32_t surface_id,
                           uint32_t buffer_id, uint32_t w, uint32_t h) {
    if (client == nullptr || surface_id == 0 || buffer_id == 0) {
        return false;
    }
    khr_wl_msg_buf_t out = {};
    khr_wl_buf_init(&out);
    /* attach(buffer, 0, 0): 8 + 12 = 20 */
    if (!khr_wl_encode_header(&out, surface_id, KHR_WL_SURFACE_ATTACH, 20) ||
        !khr_wl_encode_u32(&out, buffer_id) ||
        !khr_wl_encode_i32(&out, 0) ||
        !khr_wl_encode_i32(&out, 0)) {
        return false;
    }
    /* damage(0, 0, w, h): 8 + 16 = 24 */
    if (!khr_wl_encode_header(&out, surface_id, KHR_WL_SURFACE_DAMAGE, 24) ||
        !khr_wl_encode_i32(&out, 0) ||
        !khr_wl_encode_i32(&out, 0) ||
        !khr_wl_encode_i32(&out, (int32_t)w) ||
        !khr_wl_encode_i32(&out, (int32_t)h)) {
        return false;
    }
    /* commit: 8 */
    if (!khr_wl_encode_header(&out, surface_id, KHR_WL_SURFACE_COMMIT, 8)) {
        return false;
    }
    return khr_wl_client_send_skip(client, out.data, out.size);
}

[[nodiscard]]
bool khr_shm_buffer_destroy(khr_wl_client_t* client, uint32_t buffer_id) {
    if (client == nullptr || buffer_id == 0) {
        return false;
    }
    khr_wl_msg_buf_t out = {};
    khr_wl_buf_init(&out);
    if (!khr_wl_encode_header(&out, buffer_id, KHR_WL_BUFFER_DESTROY, 8)) {
        return false;
    }
    return khr_wl_client_send_skip(client, out.data, out.size);
}

void khr_shm_pool_destroy(khr_wl_client_t* client, khr_shm_pool_t* pool) {
    if (pool == nullptr) {
        return;
    }
    if (client != nullptr && pool->pool_id != 0) {
        khr_wl_msg_buf_t out = {};
        khr_wl_buf_init(&out);
        if (khr_wl_encode_header(&out, pool->pool_id, KHR_WL_SHM_POOL_DESTROY, 8)) {
            (void)khr_wl_client_send_skip(client, out.data, out.size);
        }
    }
    if (pool->addr != nullptr) {
        munmap(pool->addr, pool->size);
    }
    if (pool->fd >= 0) {
        close(pool->fd);
    }
    *pool = (khr_shm_pool_t){ .fd = -1 };
}

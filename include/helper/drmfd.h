#ifndef HELPER_DRMFD_H
#define HELPER_DRMFD_H

#include <stdint.h>
#include <sys/types.h>

[[nodiscard]] int wp_drm_open_render(dev_t render);
[[nodiscard]] int wp_drm_syncobj_fd_to_handle(int drm_fd, int syncobj_fd, uint32_t *handle);
[[nodiscard]] int wp_drm_syncobj_eventfd(int drm_fd, uint32_t handle, uint64_t point, int efd);
void wp_drm_syncobj_destroy(int drm_fd, uint32_t handle);

#endif /* HELPER_DRMFD_H */

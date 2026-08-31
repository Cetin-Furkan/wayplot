#define _GNU_SOURCE
#include "helper/drmfd.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <drm/drm.h>

int wp_drm_open_render(dev_t render)
{
    DIR *d;
    struct dirent *de;
    int found = -ENOENT;

    if (render == (dev_t)0)
        return -EINVAL;
    d = opendir("/dev/dri");
    if (!d)
        return -errno;
    while ((de = readdir(d))) {
        char path[280];
        struct stat st;
        int fd;

        if (strncmp(de->d_name, "renderD", 7) != 0)
            continue;
        snprintf(path, sizeof(path), "/dev/dri/%s", de->d_name);
        fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;
        if (fstat(fd, &st) == 0 && st.st_rdev == render) {
            found = fd;
            break;
        }
        close(fd);
    }
    closedir(d);
    return found;
}

int wp_drm_syncobj_fd_to_handle(int drm_fd, int syncobj_fd, uint32_t *handle)
{
    struct drm_syncobj_handle arg = {
        .fd = syncobj_fd,
        .flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_TIMELINE,
    };

    if (drm_fd < 0 || syncobj_fd < 0 || !handle)
        return -EINVAL;
    if (ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &arg) == 0) {
        *handle = arg.handle;
        return 0;
    }
    arg.flags = 0;
    arg.handle = 0;
    if (ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &arg) < 0)
        return -errno;
    *handle = arg.handle;
    return 0;
}

int wp_drm_syncobj_eventfd(int drm_fd, uint32_t handle, uint64_t point, int efd)
{
    struct drm_syncobj_eventfd arg = {
        .handle = handle,
        .flags = 0,
        .point = point,
        .fd = efd,
    };

    if (drm_fd < 0 || efd < 0 || handle == 0)
        return -EINVAL;
    if (ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_EVENTFD, &arg) < 0)
        return -errno;
    return 0;
}

void wp_drm_syncobj_destroy(int drm_fd, uint32_t handle)
{
    struct drm_syncobj_destroy arg = { .handle = handle };

    if (drm_fd < 0 || handle == 0)
        return;
    (void)ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_DESTROY, &arg);
}

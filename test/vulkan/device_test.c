#define _GNU_SOURCE
#include "vulkan/device.h"
#include "wayland/session.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int g_fail;

static void expect(int cond, const char *what)
{
    if (cond)
        printf("PASS  %s\n", what);
    else {
        printf("FAIL  %s\n", what);
        g_fail++;
    }
}

int main(void)
{
    struct wp_session s;
    struct wp_device d;
    int ret;

    ret = wp_session_open(&s);
    expect(ret == 0, "session_open");
    if (ret < 0)
        return 1;
    ret = wp_session_setup_surface(&s);
    expect(ret == 0, "surface + feedback");
    if (ret < 0) {
        wp_session_close(&s);
        return 1;
    }

    ret = wp_device_open(&d, s.fb.main_device);
    expect(ret == 0, "vkCreateDevice 1.4 with used features");
    if (ret < 0) {
        printf("      %s\n", strerror(-ret));
        wp_session_close(&s);
        return 1;
    }
    wp_device_print(&d, stdout);
    expect(d.device != VK_NULL_HANDLE, "VkDevice");
    expect(d.push_descriptor && d.push_layout && d.push_set, "push descriptor set layout + pipeline layout");
    expect(d.max_push_descriptors >= 2, "maxPushDescriptors >= 2");
    expect(d.maintenance5, "maintenance5 enabled (vkGetDeviceImageMemoryRequirements)");
    expect(d.acquire_sem != VK_NULL_HANDLE && d.acquire_fd >= 0, "acquire timeline exported as opaque fd");
    expect(d.drm_fd >= 0, "drm render node open for SYNCOBJ_EVENTFD");
    expect(d.host_image_copy, "hostImageCopy advertised (proved with vkCopyMemoryToImage round-trip)");
    expect(d.gfx != VK_NULL_HANDLE, "graphics queue");

    wp_device_close(&d);
    wp_session_close(&s);
    printf("%s  %d failure(s)\n", g_fail ? "RESULT FAIL" : "RESULT PASS", g_fail);
    return g_fail ? 1 : 0;
}

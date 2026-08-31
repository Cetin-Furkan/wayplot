#define _GNU_SOURCE
#include "vulkan/gpu.h"
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
    struct wp_gpu_caps gpu;
    int ret;

    ret = wp_session_open(&s);
    expect(ret == 0, "session_open");
    if (ret < 0) {
        printf("      %s\n", strerror(-ret));
        return 1;
    }
    ret = wp_session_setup_surface(&s);
    expect(ret == 0, "surface + dma-buf feedback");
    if (ret < 0) {
        printf("      %s\n", strerror(-ret));
        wp_session_close(&s);
        return 1;
    }

    ret = wp_gpu_enumerate(&gpu);
    expect(ret == 0 && gpu.n > 0, "vkCreateInstance 1.4 + enumerate");
    if (ret < 0) {
        printf("      enumerate %s\n", strerror(-ret));
        wp_session_close(&s);
        return 1;
    }

    ret = wp_gpu_pick(&gpu, s.fb.main_device);
    wp_gpu_caps_print(&gpu, stdout);
    expect(ret == 0, "physical device DRM node matches feedback.main_device");
    if (ret == 0) {
        const struct wp_gpu_info *g = &gpu.v[gpu.picked_index];
        expect(g->api_1_4, "picked GPU is Vulkan 1.4");
        expect(g->dma_buf && g->drm_modifier, "picked GPU exports dma-buf + modifiers");
        expect(g->push_descriptor, "pushDescriptor advertised");
        printf("      main_device %lu  render %lu  primary %lu\n",
               (unsigned long)s.fb.main_device,
               (unsigned long)g->render, (unsigned long)g->primary);
    }

    wp_gpu_caps_free(&gpu);
    wp_session_close(&s);
    printf("%s  %d failure(s)\n", g_fail ? "RESULT FAIL" : "RESULT PASS", g_fail);
    return g_fail ? 1 : 0;
}

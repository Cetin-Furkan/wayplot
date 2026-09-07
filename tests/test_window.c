#include "test_framework.h"
#include "khoros/core/config.h"
#include "khoros/wayland/xdg.h"
#include "khoros/gfx/pipeline.h"
#include <math.h>
#include <string.h>

[[nodiscard]]
bool test_window_hit_chrome_regions(void) {
    constexpr uint32_t w = 400;
    constexpr uint32_t h = 300;
    TEST_ASSERT_EQ((uint32_t)khr_window_hit(0, 0, w, h, false), (uint32_t)KHR_HIT_NW,
                   "top-left extra is NW resize");
    TEST_ASSERT_EQ((uint32_t)khr_window_hit((int32_t)(w - 1), 0, w, h, false),
                   (uint32_t)KHR_HIT_NE, "top-right extra is NE resize");
    TEST_ASSERT_EQ((uint32_t)khr_window_hit(0, (int32_t)(h - 1), w, h, false),
                   (uint32_t)KHR_HIT_SW, "bottom-left extra is SW resize");
    TEST_ASSERT_EQ((uint32_t)khr_window_hit((int32_t)(w - 1), (int32_t)(h - 1), w, h, false),
                   (uint32_t)KHR_HIT_SE, "bottom-right extra is SE resize");
    TEST_ASSERT_EQ((uint32_t)khr_window_hit((int32_t)w / 2, 4, w, h, false),
                   (uint32_t)KHR_HIT_MOVE, "top 32px bar is drag");
    TEST_ASSERT_EQ((uint32_t)khr_window_hit(2, 80, w, h, false),
                   (uint32_t)KHR_HIT_W, "left edge resize");
    TEST_ASSERT_EQ((uint32_t)khr_window_hit((int32_t)(w - 2), 80, w, h, false),
                   (uint32_t)KHR_HIT_E, "right edge resize");
    TEST_ASSERT_EQ((uint32_t)khr_window_hit(80, (int32_t)(h - 2), w, h, false),
                   (uint32_t)KHR_HIT_S, "bottom edge resize");
    TEST_ASSERT_EQ((uint32_t)khr_window_hit(80, 80, w, h, false),
                   (uint32_t)KHR_HIT_CLIENT, "interior is client");
    TEST_ASSERT_EQ((uint32_t)khr_window_hit(0, 0, w, h, true),
                   (uint32_t)KHR_HIT_CLIENT, "fullscreen has no chrome");
    TEST_ASSERT_EQ(khr_hit_resize_edge(KHR_HIT_SE), KHR_XDG_RESIZE_BOTTOM_RIGHT,
                   "SE maps to xdg edge");
    TEST_ASSERT_EQ(khr_hit_resize_edge(KHR_HIT_MOVE), KHR_XDG_RESIZE_NONE,
                   "move is not a resize edge");
    return true;
}

[[nodiscard]]
bool test_window_buffer_size_from_xdg(void) {
    khr_xdg_shell_t shell = {};
    khr_xdg_init(&shell);
    uint32_t w = 0, h = 0;
    khr_window_buffer_size(&shell, &w, &h);
    TEST_ASSERT_EQ(w, KHR_WINDOW_DEFAULT_W, "unset configure uses default width");
    TEST_ASSERT_EQ(h, KHR_WINDOW_DEFAULT_H, "unset configure uses default height");
    shell.width = 1280;
    shell.height = 720;
    khr_window_buffer_size(&shell, &w, &h);
    TEST_ASSERT_EQ(w, 1280U, "xdg width is the buffer width");
    TEST_ASSERT_EQ(h, 720U, "xdg height is the buffer height");
    shell.width = 1;
    shell.height = 1;
    khr_window_buffer_size(&shell, &w, &h);
    TEST_ASSERT_EQ(w, 1U, "compositor size is used verbatim (ack match)");
    TEST_ASSERT_EQ(h, 1U, "compositor size is used verbatim (ack match)");
    TEST_ASSERT(KHR_WINDOW_CHROME_TOP == 32, "drag bar is 32px");
    TEST_ASSERT(KHR_WINDOW_CHROME_CORNER >= KHR_WINDOW_CHROME_EDGE,
                "corners are extra beyond the edge strip");
    return true;
}

[[nodiscard]]
bool test_plot_demo_samples_in_range(void) {
    TEST_ASSERT(KHR_PLOT_SAMPLE_COUNT >= 2U, "ribbon needs at least two samples");
    float s[KHR_PLOT_SAMPLE_COUNT];
    memset(s, 0, sizeof(s));
    khr_plot_fill_demo_samples(nullptr, KHR_PLOT_SAMPLE_COUNT);
    khr_plot_fill_demo_samples(s, 0);
    khr_plot_fill_demo_samples(s, KHR_PLOT_SAMPLE_COUNT);
    float min_v = s[0];
    float max_v = s[0];
    for (uint32_t i = 0; i < KHR_PLOT_SAMPLE_COUNT; i++) {
        TEST_ASSERT(isfinite(s[i]), "sample must be finite");
        TEST_ASSERT(s[i] > 0.0f && s[i] < 1.0f, "demo series stays in (0,1)");
        if (s[i] < min_v) {
            min_v = s[i];
        }
        if (s[i] > max_v) {
            max_v = s[i];
        }
    }
    TEST_ASSERT(max_v - min_v > 0.2f, "demo series is not a flat line");
    TEST_ASSERT(s[0] != s[KHR_PLOT_SAMPLE_COUNT / 4U],
                "series varies across the window");
    return true;
}

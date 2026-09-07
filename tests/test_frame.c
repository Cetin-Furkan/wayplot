#include "test_framework.h"
#include "khoros/gfx/device.h"
#include "khoros/gfx/bda_arena.h"
#include "khoros/gfx/frame.h"
#include <stdio.h>
#include <sys/sysmacros.h>

/*
 * The anti-facade test: a command buffer that is actually submitted to the
 * queue, synchronized with a timeline semaphore, and verified by readback.
 * A red center pixel proves dynamic rendering + viewport/scissor + BDA push
 * constants + host->shader barrier + submit2 + timeline wait all executed on
 * silicon. No GPU in the test environment is an honest SKIP (device init is
 * the gate), never a vacuous pass: every run prints which readback path ran.
 */

[[nodiscard]]
bool test_gfx_real_submit_and_readback(void) {
    khr_gfx_device_t dev = {};
    if (!khr_gfx_device_init(&dev, (dev_t)0)) {
        TEST_SKIP("no Vulkan device in this environment");
        return true;
    }

    printf("[m5=%d m6=%d host_copy=%d push_desc=%d] ",
           dev.has_maintenance5, dev.has_maintenance6,
           dev.has_host_image_copy, dev.has_push_descriptor);

    khr_bda_arena_t arena = {};
    TEST_ASSERT(khr_bda_arena_init(&dev, &arena, 64 * 1024), "arena init failed");

    khr_gfx_frame_t frame = {};
    TEST_ASSERT(khr_gfx_frame_init(&dev, &frame, KHR_FRAME_DEFAULT_W,
                                   KHR_FRAME_DEFAULT_H), "frame init failed");
    printf("[readback=%s] ", frame.use_host_copy ? "host-copy" : "transfer");

    uint8_t bgra[4] = {};
    TEST_ASSERT(khr_gfx_frame_render_red_card(&dev, &frame, &arena, bgra),
                "real submit + readback failed");
    TEST_ASSERT_EQ(frame.next_point, 2U, "exactly one timeline point must advance");

    /* Fullscreen opaque red card over dark-blue clear: center must be red. */
    TEST_ASSERT_GT(bgra[2], 200U, "center pixel red channel too dark");
    TEST_ASSERT_LT(bgra[1], 80U, "center pixel green channel too bright");
    TEST_ASSERT_LT(bgra[0], 80U, "center pixel blue channel too bright");
    TEST_ASSERT_EQ(bgra[3], 255U, "center pixel alpha must be opaque");

    khr_gfx_frame_destroy(&dev, &frame);
    khr_bda_arena_destroy(&dev, &arena);
    khr_gfx_device_destroy(&dev);
    return true;
}

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

/*
 * Hardware Depth Buffer (Reversed-Z) Occlusion Test:
 * Submits two overlapping triangles:
 *   - Red triangle at Z = 0.2 (Far in Reversed-Z)
 *   - Green triangle at Z = 0.8 (Near in Reversed-Z)
 * In Reversed-Z, Near maps to 1.0 and Far maps to 0.0 with compare op >=.
 * Therefore, Z = 0.8 occludes Z = 0.2 regardless of submission order:
 *   Pass 1 (Far first, Near second): Red is drawn, then Green overwrites Red (0.8 >= 0.2).
 *   Pass 2 (Near first, Far second): Green is drawn, then Red fails test (0.2 < 0.8) and is culled by Early-Z.
 * Both passes MUST yield Green center pixels on silicon readback!
 */
[[nodiscard]]
bool test_vulkan_depth_reversed_z_occlusion(void) {
    khr_gfx_device_t dev = {};
    if (!khr_gfx_device_init(&dev, (dev_t)0)) {
        TEST_SKIP("no Vulkan device in this environment");
        return true;
    }

    VkFormat depth_fmt = khr_gfx_depth_format(&dev);
    if (depth_fmt == VK_FORMAT_UNDEFINED) {
        TEST_SKIP("no supported hardware depth format on this device");
        khr_gfx_device_destroy(&dev);
        return true;
    }

    TEST_ASSERT(depth_fmt == VK_FORMAT_D32_SFLOAT ||
                depth_fmt == VK_FORMAT_D24_UNORM_S8_UINT ||
                depth_fmt == VK_FORMAT_D32_SFLOAT_S8_UINT,
                "depth format must be 32-bit float or 24-bit unorm");

    khr_bda_arena_t arena = {};
    TEST_ASSERT(khr_bda_arena_init(&dev, &arena, 64 * 1024), "arena init failed");

    khr_gfx_frame_t frame = {};
    TEST_ASSERT(khr_gfx_frame_init(&dev, &frame, 64, 64), "frame init failed");

    /* Pass 1: Submit Far first (Z=0.2 Red), then Near second (Z=0.8 Green) */
    uint8_t bgra1[4] = {};
    TEST_ASSERT(khr_gfx_frame_render_depth_test(&dev, &frame, &arena, false, bgra1),
                "render Far-first depth test failed");

    /* Center pixel must be Green: Green channel high, Red/Blue low */
    TEST_ASSERT_GT(bgra1[1], 200U, "Pass 1 (Far->Near): Green channel must be bright (>200)");
    TEST_ASSERT_LT(bgra1[2], 50U,  "Pass 1 (Far->Near): Red channel must be dark (<50)");
    TEST_ASSERT_LT(bgra1[0], 50U,  "Pass 1 (Far->Near): Blue channel must be dark (<50)");
    TEST_ASSERT_EQ(bgra1[3], 255U, "Pass 1 (Far->Near): Alpha must be opaque (255)");

    /* Pass 2: Submit Near first (Z=0.8 Green), then Far second (Z=0.2 Red) */
    uint8_t bgra2[4] = {};
    TEST_ASSERT(khr_gfx_frame_render_depth_test(&dev, &frame, &arena, true, bgra2),
                "render Near-first depth test failed");

    /* Center pixel must STILL be Green because Red at Z=0.2 was culled by Reversed-Z depth test */
    TEST_ASSERT_GT(bgra2[1], 200U, "Pass 2 (Near->Far): Green channel must remain bright (>200)");
    TEST_ASSERT_LT(bgra2[2], 50U,  "Pass 2 (Near->Far): Red channel must not overwrite Green (<50)");
    TEST_ASSERT_LT(bgra2[0], 50U,  "Pass 2 (Near->Far): Blue channel must remain dark (<50)");
    TEST_ASSERT_EQ(bgra2[3], 255U, "Pass 2 (Near->Far): Alpha must be opaque (255)");

    khr_gfx_frame_destroy(&dev, &frame);
    khr_bda_arena_destroy(&dev, &arena);
    khr_gfx_device_destroy(&dev);
    return true;
}

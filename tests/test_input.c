#include "test_framework.h"
#include "khoros/core/input.h"
#include "khoros/wayland/seat.h"
#include "khoros/wayland/wire.h"

#include <math.h>
#include <string.h>
#include <pthread.h>

typedef struct {
    khr_input_ring_t* ring;
    uint32_t          count;
} thread_worker_ctx_t;

static void* input_producer_thread(void* arg) {
    thread_worker_ctx_t* ctx = (thread_worker_ctx_t*)arg;
    for (uint32_t i = 0; i < ctx->count; i++) {
        khr_input_event_t ev = {
            .type    = KHR_INPUT_KEY,
            .code    = i,
            .state   = i % 2,
            .val_i32 = (int32_t)i,
        };
        /* Spin until pushed (bounded) */
        while (!khr_input_ring_push(ctx->ring, &ev)) {
            sched_yield();
        }
    }
    return nullptr;
}

[[nodiscard]]
bool test_input_ring_spsc_lockfree_stress(void) {
    khr_input_ring_t ring = {};
    khr_input_ring_init(&ring);

    constexpr uint32_t TOTAL_EVENTS = 500;
    thread_worker_ctx_t ctx = {
        .ring  = &ring,
        .count = TOTAL_EVENTS,
    };

    pthread_t thread;
    TEST_ASSERT_EQ(pthread_create(&thread, nullptr, input_producer_thread, &ctx), 0, "create producer");

    uint32_t received = 0;
    while (received < TOTAL_EVENTS) {
        khr_input_event_t ev = {};
        if (khr_input_ring_pop(&ring, &ev)) {
            TEST_ASSERT_EQ(ev.code, received, "FIFO ordering match");
            TEST_ASSERT_EQ(ev.val_i32, (int32_t)received, "Event payload match");
            received++;
        } else {
            sched_yield();
        }
    }

    TEST_ASSERT_EQ(pthread_join(thread, nullptr), 0, "join producer");
    TEST_ASSERT_EQ(received, TOTAL_EVENTS, "All 500 events received cleanly");

    return true;
}

[[nodiscard]]
bool test_action_map_digital_edge_transitions(void) {
    khr_input_ring_t ring = {};
    khr_input_ring_init(&ring);

    khr_action_map_t map = {};
    khr_action_map_init(&map);

    constexpr uint32_t ACTION_JUMP = 0;
    constexpr uint32_t KEY_SPACE   = 57;
    khr_action_map_bind_digital(&map, ACTION_JUMP, KHR_INPUT_KEY, KEY_SPACE);

    /* Tick 1: Initial idle state */
    khr_action_map_tick(&map, &ring, 1.0f / 120.0f);
    TEST_ASSERT(!khr_action_map_is_down(&map, ACTION_JUMP), "Not down at start");
    TEST_ASSERT(!khr_action_map_just_pressed(&map, ACTION_JUMP), "Not just pressed");
    TEST_ASSERT(!khr_action_map_just_released(&map, ACTION_JUMP), "Not just released");

    /* Push key press event */
    khr_input_event_t ev_press = {
        .type  = KHR_INPUT_KEY,
        .code  = KEY_SPACE,
        .state = 1,
    };
    TEST_ASSERT(khr_input_ring_push(&ring, &ev_press), "push press");

    /* Tick 2: Pressed transition */
    khr_action_map_tick(&map, &ring, 1.0f / 120.0f);
    TEST_ASSERT(khr_action_map_is_down(&map, ACTION_JUMP), "Down on tick 2");
    TEST_ASSERT(khr_action_map_just_pressed(&map, ACTION_JUMP), "Just pressed on tick 2");
    TEST_ASSERT(!khr_action_map_just_released(&map, ACTION_JUMP), "Not just released");

    /* Tick 3: Hold state (no new events) */
    khr_action_map_tick(&map, &ring, 1.0f / 120.0f);
    TEST_ASSERT(khr_action_map_is_down(&map, ACTION_JUMP), "Still down on tick 3");
    TEST_ASSERT(!khr_action_map_just_pressed(&map, ACTION_JUMP), "No longer just pressed");
    TEST_ASSERT(!khr_action_map_just_released(&map, ACTION_JUMP), "Not released");

    /* Push key release event */
    khr_input_event_t ev_release = {
        .type  = KHR_INPUT_KEY,
        .code  = KEY_SPACE,
        .state = 0,
    };
    TEST_ASSERT(khr_input_ring_push(&ring, &ev_release), "push release");

    /* Tick 4: Released transition */
    khr_action_map_tick(&map, &ring, 1.0f / 120.0f);
    TEST_ASSERT(!khr_action_map_is_down(&map, ACTION_JUMP), "Not down on tick 4");
    TEST_ASSERT(!khr_action_map_just_pressed(&map, ACTION_JUMP), "Not pressed");
    TEST_ASSERT(khr_action_map_just_released(&map, ACTION_JUMP), "Just released on tick 4");

    /* Tick 5: Back to idle */
    khr_action_map_tick(&map, &ring, 1.0f / 120.0f);
    TEST_ASSERT(!khr_action_map_is_down(&map, ACTION_JUMP), "Idle on tick 5");
    TEST_ASSERT(!khr_action_map_just_pressed(&map, ACTION_JUMP), "Not pressed");
    TEST_ASSERT(!khr_action_map_just_released(&map, ACTION_JUMP), "Not released");

    return true;
}

[[nodiscard]]
bool test_action_map_analog_deadzone_and_curves(void) {
    khr_input_ring_t ring = {};
    khr_input_ring_init(&ring);

    khr_action_map_t map = {};
    khr_action_map_init(&map);

    constexpr uint32_t AXIS_LOOK_X = 0;
    khr_action_map_bind_axis_mouse(&map, AXIS_LOOK_X, KHR_AXIS_SRC_MOUSE_DX, 0.01f);
    khr_action_map_set_axis_deadzone(&map, AXIS_LOOK_X, 0.2f, 2.0f); /* Deadzone 0.2, quadratic curve */

    /* 1. Motion within deadzone: dx = 10 -> raw = 10 * 0.01 = 0.1 <= 0.2 -> 0.0 */
    khr_input_event_t ev1 = { .type = KHR_INPUT_MOTION, .dx = 10, .dy = 0 };
    (void)khr_input_ring_push(&ring, &ev1);
    khr_action_map_tick(&map, &ring, 1.0f / 120.0f);
    TEST_ASSERT_EQ(map.axes[AXIS_LOOK_X].current_val, 0.0f, "Within deadzone yields 0.0");

    /* 2. Motion above deadzone: dx = 60 -> raw = 0.6 > 0.2.
     * Remapped: (0.6 - 0.2) / (1.0 - 0.2) = 0.4 / 0.8 = 0.5.
     * Quadratic curve: 0.5^2 = 0.25 */
    khr_input_event_t ev2 = { .type = KHR_INPUT_MOTION, .dx = 60, .dy = 0 };
    (void)khr_input_ring_push(&ring, &ev2);
    khr_action_map_tick(&map, &ring, 1.0f / 120.0f);
    TEST_ASSERT_LT(fabsf(map.axes[AXIS_LOOK_X].current_val - 0.25f), 0.001f, "Remapped quadratic response 0.25");

    /* 3. Negative motion: dx = -60 -> remapped = -0.5 -> quadratic curve with sign = -0.25 */
    khr_input_event_t ev3 = { .type = KHR_INPUT_MOTION, .dx = -60, .dy = 0 };
    (void)khr_input_ring_push(&ring, &ev3);
    khr_action_map_tick(&map, &ring, 1.0f / 120.0f);
    TEST_ASSERT_LT(fabsf(map.axes[AXIS_LOOK_X].current_val - (-0.25f)), 0.001f, "Symmetric negative response -0.25");

    return true;
}

[[nodiscard]]
bool test_action_map_temporal_smoothing(void) {
    khr_input_ring_t ring = {};
    khr_input_ring_init(&ring);

    khr_action_map_t map = {};
    khr_action_map_init(&map);

    constexpr uint32_t AXIS_SMOOTH = 1;
    khr_action_map_bind_axis_mouse(&map, AXIS_SMOOTH, KHR_AXIS_SRC_MOUSE_DY, 0.01f);
    khr_action_map_set_axis_smoothing(&map, AXIS_SMOOTH, 0.25f); /* alpha = 0.25 */

    /* Initial state = 0.0 */
    /* Push input of 100 -> raw = 1.0 */
    khr_input_event_t ev1 = { .type = KHR_INPUT_MOTION, .dx = 0, .dy = 100 };
    (void)khr_input_ring_push(&ring, &ev1);
    khr_action_map_tick(&map, &ring, 1.0f / 120.0f);
    /* val_1 = 0.0 + 0.25 * (1.0 - 0.0) = 0.25 */
    TEST_ASSERT_LT(fabsf(map.axes[AXIS_SMOOTH].current_val - 0.25f), 0.001f, "Smoothing step 1: 0.25");

    /* Second tick with same target: val_2 = 0.25 + 0.25 * (1.0 - 0.25) = 0.4375 */
    khr_input_event_t ev2 = { .type = KHR_INPUT_MOTION, .dx = 0, .dy = 100 };
    (void)khr_input_ring_push(&ring, &ev2);
    khr_action_map_tick(&map, &ring, 1.0f / 120.0f);
    TEST_ASSERT_LT(fabsf(map.axes[AXIS_SMOOTH].current_val - 0.4375f), 0.001f, "Smoothing step 2: 0.4375");

    return true;
}

[[nodiscard]]
bool test_action_map_fixed_tick_subtick_interpolation(void) {
    khr_action_map_t map = {};
    khr_action_map_init(&map);

    constexpr uint32_t AXIS_INTERP = 2;
    map.axes[AXIS_INTERP].previous_val = 10.0f;
    map.axes[AXIS_INTERP].current_val  = 20.0f;

    /* Render thread queries sub-tick interpolation across alpha */
    float s0   = khr_action_map_sample_axis(&map, AXIS_INTERP, 0.0f);
    float s25  = khr_action_map_sample_axis(&map, AXIS_INTERP, 0.25f);
    float s50  = khr_action_map_sample_axis(&map, AXIS_INTERP, 0.5f);
    float s75  = khr_action_map_sample_axis(&map, AXIS_INTERP, 0.75f);
    float s100 = khr_action_map_sample_axis(&map, AXIS_INTERP, 1.0f);

    TEST_ASSERT_LT(fabsf(s0   - 10.0f), 0.001f, "alpha 0.0 = 10.0");
    TEST_ASSERT_LT(fabsf(s25  - 12.5f), 0.001f, "alpha 0.25 = 12.5");
    TEST_ASSERT_LT(fabsf(s50  - 15.0f), 0.001f, "alpha 0.50 = 15.0");
    TEST_ASSERT_LT(fabsf(s75  - 17.5f), 0.001f, "alpha 0.75 = 17.5");
    TEST_ASSERT_LT(fabsf(s100 - 20.0f), 0.001f, "alpha 1.00 = 20.0");

    return true;
}

[[nodiscard]]
bool test_wayland_seat_input_ring_forwarding(void) {
    khr_input_ring_t ring = {};
    khr_input_ring_init(&ring);

    khr_seat_t seat = {};
    khr_seat_init(&seat);
    khr_seat_attach_input_ring(&seat, &ring);
    seat.seat_id     = 1;
    seat.pointer_id  = 10;
    seat.keyboard_id = 11;

    /* Synthesize Wayland wire packet with:
     * 1. Motion event on pointer_id 10 (fx = 25600 -> x=100px, fy = 51200 -> y=200px)
     * 2. Button event on pointer_id 10 (KHR_BTN_LEFT, pressed)
     * 3. Key event on keyboard_id 11 (Key 17, pressed)
     */
    khr_wl_msg_buf_t buf = {};
    khr_wl_buf_init(&buf);

    /* Pointer Motion */
    TEST_ASSERT(khr_wl_encode_header(&buf, 10, KHR_WL_POINTER_EVENT_MOTION, 20), "encode motion hdr");
    TEST_ASSERT(khr_wl_encode_u32(&buf, 1000), "encode time");
    TEST_ASSERT(khr_wl_encode_i32(&buf, 25'600), "encode fx");
    TEST_ASSERT(khr_wl_encode_i32(&buf, 51'200), "encode fy");

    /* Pointer Button */
    TEST_ASSERT(khr_wl_encode_header(&buf, 10, KHR_WL_POINTER_EVENT_BUTTON, 24), "encode button hdr");
    TEST_ASSERT(khr_wl_encode_u32(&buf, 1), "encode serial");
    TEST_ASSERT(khr_wl_encode_u32(&buf, 1001), "encode time");
    TEST_ASSERT(khr_wl_encode_u32(&buf, KHR_BTN_LEFT), "encode btn");
    TEST_ASSERT(khr_wl_encode_u32(&buf, KHR_WL_POINTER_PRESSED), "encode state");

    /* Keyboard Key */
    TEST_ASSERT(khr_wl_encode_header(&buf, 11, KHR_WL_KEYBOARD_EVENT_KEY, 24), "encode key hdr");
    TEST_ASSERT(khr_wl_encode_u32(&buf, 2), "encode serial");
    TEST_ASSERT(khr_wl_encode_u32(&buf, 1002), "encode time");
    TEST_ASSERT(khr_wl_encode_u32(&buf, 17), "encode key");
    TEST_ASSERT(khr_wl_encode_u32(&buf, KHR_WL_KEY_PRESSED), "encode state");

    /* Consume buffer via khr_seat_consume */
    uint32_t count = khr_seat_consume(nullptr, &seat, buf.data, buf.size);
    TEST_ASSERT_EQ(count, 3U, "Consumed 3 events");

    /* Verify ring received all 3 events in order */
    khr_input_event_t ev1 = {};
    TEST_ASSERT(khr_input_ring_pop(&ring, &ev1), "pop ev1");
    TEST_ASSERT_EQ(ev1.type, (uint32_t)KHR_INPUT_MOTION, "ev1 is motion");
    TEST_ASSERT_EQ(ev1.x, 100, "ev1 x=100");
    TEST_ASSERT_EQ(ev1.y, 200, "ev1 y=200");

    khr_input_event_t ev2 = {};
    TEST_ASSERT(khr_input_ring_pop(&ring, &ev2), "pop ev2");
    TEST_ASSERT_EQ(ev2.type, (uint32_t)KHR_INPUT_BUTTON, "ev2 is button");
    TEST_ASSERT_EQ(ev2.code, KHR_BTN_LEFT, "ev2 code is BTN_LEFT");
    TEST_ASSERT_EQ(ev2.state, 1U, "ev2 state is pressed");

    khr_input_event_t ev3 = {};
    TEST_ASSERT(khr_input_ring_pop(&ring, &ev3), "pop ev3");
    TEST_ASSERT_EQ(ev3.type, (uint32_t)KHR_INPUT_KEY, "ev3 is key");
    TEST_ASSERT_EQ(ev3.code, 17U, "ev3 code is key 17");
    TEST_ASSERT_EQ(ev3.state, 1U, "ev3 state is pressed");

    return true;
}

[[nodiscard]]
bool test_action_map_digital_axis_raw_keys(void) {
    khr_input_ring_t ring = {};
    khr_input_ring_init(&ring);

    khr_action_map_t map = {};
    khr_action_map_init(&map);

    constexpr uint32_t AXIS_MOVE_X = 0;
    constexpr uint32_t KEY_D       = 32;
    constexpr uint32_t KEY_A       = 30;

    /* Bind digital axis directly with raw keycodes without pre-binding actions */
    khr_action_map_bind_axis_digital(&map, AXIS_MOVE_X, KEY_D, KEY_A, 1.0f);

    /* 1. Initial tick: neutral (0.0) */
    khr_action_map_tick(&map, &ring, 1.0f / 120.0f);
    TEST_ASSERT_EQ(map.axes[AXIS_MOVE_X].current_val, 0.0f, "Initial digital axis is 0.0");

    /* 2. Press KEY_D (+X) */
    khr_input_event_t ev_d = {
        .type  = KHR_INPUT_KEY,
        .code  = KEY_D,
        .state = 1,
    };
    TEST_ASSERT(khr_input_ring_push(&ring, &ev_d), "push KEY_D press");
    khr_action_map_tick(&map, &ring, 1.0f / 120.0f);
    TEST_ASSERT_EQ(map.axes[AXIS_MOVE_X].current_val, 1.0f, "KEY_D pressed produces +1.0");

    /* 3. Release KEY_D */
    ev_d.state = 0;
    TEST_ASSERT(khr_input_ring_push(&ring, &ev_d), "push KEY_D release");
    khr_action_map_tick(&map, &ring, 1.0f / 120.0f);
    TEST_ASSERT_EQ(map.axes[AXIS_MOVE_X].current_val, 0.0f, "KEY_D released returns to 0.0");

    /* 4. Press KEY_A (-X) */
    khr_input_event_t ev_a = {
        .type  = KHR_INPUT_KEY,
        .code  = KEY_A,
        .state = 1,
    };
    TEST_ASSERT(khr_input_ring_push(&ring, &ev_a), "push KEY_A press");
    khr_action_map_tick(&map, &ring, 1.0f / 120.0f);
    TEST_ASSERT_EQ(map.axes[AXIS_MOVE_X].current_val, -1.0f, "KEY_A pressed produces -1.0");

    /* 5. Press KEY_D simultaneously (canceling out) */
    ev_d.state = 1;
    TEST_ASSERT(khr_input_ring_push(&ring, &ev_d), "push KEY_D press simultaneous");
    khr_action_map_tick(&map, &ring, 1.0f / 120.0f);
    TEST_ASSERT_EQ(map.axes[AXIS_MOVE_X].current_val, 0.0f, "KEY_A and KEY_D simultaneous cancel to 0.0");

    return true;
}

[[nodiscard]]
bool test_input_ring_burst_saturation_and_overflow(void) {
    khr_input_ring_t ring = {};
    khr_input_ring_init(&ring);
    TEST_ASSERT_EQ(khr_input_ring_overflow_count(&ring), 0U, "Initial overflow count is 0");

    /* Fill ring capacity (KHR_INPUT_RING_CAP = 256) */
    for (uint32_t i = 0; i < KHR_INPUT_RING_CAP; i++) {
        khr_input_event_t ev = { .type = KHR_INPUT_KEY, .code = i, .state = 1 };
        TEST_ASSERT(khr_input_ring_push(&ring, &ev), "push within capacity");
    }

    /* Push 50 overflowing events */
    constexpr uint32_t OVERFLOW_BURST = 50;
    for (uint32_t i = 0; i < OVERFLOW_BURST; i++) {
        khr_input_event_t ev = { .type = KHR_INPUT_KEY, .code = 1000 + i, .state = 1 };
        TEST_ASSERT(!khr_input_ring_push(&ring, &ev), "overflow push rejected");
    }
    TEST_ASSERT_EQ(khr_input_ring_overflow_count(&ring), OVERFLOW_BURST, "Overflow count matches 50 dropped");

    /* Drain 50 events to make room */
    khr_input_event_t drained[50] = {};
    uint32_t n = khr_input_ring_drain(&ring, drained, 50);
    TEST_ASSERT_EQ(n, 50U, "Drained 50 events");

    /* Push 50 new events now succeeds */
    for (uint32_t i = 0; i < 50; i++) {
        khr_input_event_t ev = { .type = KHR_INPUT_KEY, .code = 2000 + i, .state = 1 };
        TEST_ASSERT(khr_input_ring_push(&ring, &ev), "push after drain succeeds");
    }

    return true;
}

[[nodiscard]]
bool test_action_map_unbounded_mouse_clamp(void) {
    khr_input_ring_t ring = {};
    khr_input_ring_init(&ring);

    khr_action_map_t map = {};
    khr_action_map_init(&map);

    constexpr uint32_t AXIS_LOOK_X = 0;
    khr_action_map_bind_axis_mouse(&map, AXIS_LOOK_X, KHR_AXIS_SRC_MOUSE_DX, 0.01f);

    /* Push large delta: dx = 250 -> raw = 2.5. Default mouse binding allows > 1.0 */
    khr_input_event_t ev1 = { .type = KHR_INPUT_MOTION, .dx = 250, .dy = 0 };
    TEST_ASSERT(khr_input_ring_push(&ring, &ev1), "push large motion");
    khr_action_map_tick(&map, &ring, 1.0f / 120.0f);
    TEST_ASSERT_GT(map.axes[AXIS_LOOK_X].current_val, 2.0f, "Mouse look value exceeds 1.0 without hard clipping");

    /* Configure custom clamp [-0.5, +0.5] and verify clamping works */
    khr_action_map_set_axis_clamp(&map, AXIS_LOOK_X, -0.5f, 0.5f);
    TEST_ASSERT(khr_input_ring_push(&ring, &ev1), "push large motion with clamp");
    khr_action_map_tick(&map, &ring, 1.0f / 120.0f);
    TEST_ASSERT_LE(map.axes[AXIS_LOOK_X].current_val, 0.5001f, "Mouse look clamped to 0.5");

    return true;
}

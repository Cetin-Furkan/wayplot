#include "khoros/core/input.h"

#include <math.h>
#include <string.h>

void khr_input_ring_init(khr_input_ring_t* ring) {
    if (ring == nullptr) {
        return;
    }
    memset(ring, 0, sizeof(*ring));
    atomic_init(&ring->head, 0);
    atomic_init(&ring->tail, 0);
    atomic_init(&ring->overflow_count, 0);
}

[[nodiscard]]
bool khr_input_ring_push(khr_input_ring_t* ring, const khr_input_event_t* ev) {
    if (ring == nullptr || ev == nullptr) {
        return false;
    }

    uint32_t head = atomic_load_explicit(&ring->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&ring->tail, memory_order_acquire);

    if ((head - tail) >= KHR_INPUT_RING_CAP) {
        atomic_fetch_add_explicit(&ring->overflow_count, 1U, memory_order_relaxed);
        return false; /* Ring buffer full: overflow guard */
    }

    ring->events[head & (KHR_INPUT_RING_CAP - 1U)] = *ev;
    atomic_store_explicit(&ring->head, head + 1U, memory_order_release);
    return true;
}

[[nodiscard]]
bool khr_input_ring_pop(khr_input_ring_t* ring, khr_input_event_t* out_ev) {
    if (ring == nullptr || out_ev == nullptr) {
        return false;
    }

    uint32_t tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&ring->head, memory_order_acquire);

    if (tail == head) {
        return false; /* Ring buffer empty */
    }

    *out_ev = ring->events[tail & (KHR_INPUT_RING_CAP - 1U)];
    atomic_store_explicit(&ring->tail, tail + 1U, memory_order_release);
    return true;
}

uint32_t khr_input_ring_drain(khr_input_ring_t* ring, khr_input_event_t* out_events, uint32_t max_count) {
    if (ring == nullptr || out_events == nullptr || max_count == 0) {
        return 0;
    }

    uint32_t drained = 0;
    while (drained < max_count) {
        if (!khr_input_ring_pop(ring, &out_events[drained])) {
            break;
        }
        drained++;
    }
    return drained;
}

[[nodiscard]]
uint32_t khr_input_ring_overflow_count(const khr_input_ring_t* ring) {
    if (ring == nullptr) {
        return 0;
    }
    return atomic_load_explicit(&ring->overflow_count, memory_order_relaxed);
}

void khr_action_map_init(khr_action_map_t* map) {
    if (map == nullptr) {
        return;
    }
    memset(map, 0, sizeof(*map));

    for (uint32_t i = 0; i < KHR_AXIS_MAX; i++) {
        map->axes[i].scale      = 1.0f;
        map->axes[i].deadzone   = 0.0f;
        map->axes[i].curve_exp  = 1.0f;
        map->axes[i].smoothing  = 1.0f; /* 1.0 = instant response (no low-pass delay) */
        map->axes[i].min_clamp  = -1.0f;
        map->axes[i].max_clamp  =  1.0f;
    }
}

void khr_action_map_bind_digital(khr_action_map_t* map, uint32_t action_id,
                                 uint32_t input_type, uint32_t input_code) {
    if (map == nullptr || action_id >= KHR_ACTION_MAX) {
        return;
    }
    khr_action_binding_t* b = &map->actions[action_id];
    if (b->binding_count < KHR_BINDINGS_PER_ACTION) {
        b->input_types[b->binding_count] = input_type;
        b->input_codes[b->binding_count] = input_code;
        b->binding_count++;
    }
}

void khr_action_map_bind_axis_digital(khr_action_map_t* map, uint32_t axis_id,
                                      uint32_t pos_key, uint32_t neg_key, float scale) {
    if (map == nullptr || axis_id >= KHR_AXIS_MAX) {
        return;
    }
    khr_axis_binding_t* a = &map->axes[axis_id];
    a->source_type = KHR_AXIS_SRC_DIGITAL_KEYS;
    a->pos_code    = pos_key;
    a->neg_code    = neg_key;
    a->scale       = (scale != 0.0f) ? scale : 1.0f;
}

void khr_action_map_bind_axis_mouse(khr_action_map_t* map, uint32_t axis_id,
                                    uint32_t mouse_source, float sensitivity) {
    if (map == nullptr || axis_id >= KHR_AXIS_MAX) {
        return;
    }
    khr_axis_binding_t* a = &map->axes[axis_id];
    a->source_type = mouse_source;
    a->scale       = (sensitivity != 0.0f) ? sensitivity : 1.0f;
    a->min_clamp   = -10'000.0f;
    a->max_clamp   =  10'000.0f;
}

void khr_action_map_set_axis_deadzone(khr_action_map_t* map, uint32_t axis_id,
                                      float deadzone, float curve_exp) {
    if (map == nullptr || axis_id >= KHR_AXIS_MAX) {
        return;
    }
    khr_axis_binding_t* a = &map->axes[axis_id];
    a->deadzone  = (deadzone >= 0.0f && deadzone < 0.99f) ? deadzone : 0.0f;
    a->curve_exp = (curve_exp >= 1.0f) ? curve_exp : 1.0f;
}

void khr_action_map_set_axis_smoothing(khr_action_map_t* map, uint32_t axis_id,
                                       float smoothing_factor) {
    if (map == nullptr || axis_id >= KHR_AXIS_MAX) {
        return;
    }
    khr_axis_binding_t* a = &map->axes[axis_id];
    a->smoothing = (smoothing_factor > 0.0f && smoothing_factor <= 1.0f) ? smoothing_factor : 1.0f;
}

void khr_action_map_set_axis_clamp(khr_action_map_t* map, uint32_t axis_id,
                                   float min_val, float max_val) {
    if (map == nullptr || axis_id >= KHR_AXIS_MAX || min_val > max_val) {
        return;
    }
    map->axes[axis_id].min_clamp = min_val;
    map->axes[axis_id].max_clamp = max_val;
}

void khr_action_map_tick(khr_action_map_t* map, khr_input_ring_t* ring, [[maybe_unused]] float dt) {
    if (map == nullptr) {
        return;
    }

    /* 1. Advance state: copy current to previous, reset one-shot transition flags */
    for (uint32_t i = 0; i < KHR_ACTION_MAX; i++) {
        map->actions[i].just_pressed  = false;
        map->actions[i].just_released = false;
        map->actions[i].press_count   = 0;
    }

    for (uint32_t i = 0; i < KHR_AXIS_MAX; i++) {
        map->axes[i].previous_val = map->axes[i].current_val;
        map->axes[i].accum_raw    = 0.0f;
    }

    map->mouse_dx = 0;
    map->mouse_dy = 0;

    /* 2. Drain all pending input events from the ring buffer */
    khr_input_event_t ev = {};
    while (ring != nullptr && khr_input_ring_pop(ring, &ev)) {
        if (ev.type == KHR_INPUT_KEY || ev.type == KHR_INPUT_BUTTON) {
            /* Update global key/button tracking */
            if (ev.type == KHR_INPUT_KEY && ev.code < KHR_MAX_KEY_CODES) {
                map->keys_down[ev.code] = (ev.state != 0);
            } else if (ev.type == KHR_INPUT_BUTTON && ev.code < KHR_MAX_BUTTON_CODES) {
                map->buttons_down[ev.code] = (ev.state != 0);
            }

            /* Update matching digital actions */
            for (uint32_t a_idx = 0; a_idx < KHR_ACTION_MAX; a_idx++) {
                khr_action_binding_t* b = &map->actions[a_idx];
                for (uint32_t k = 0; k < b->binding_count; k++) {
                    if (b->input_types[k] == ev.type && b->input_codes[k] == ev.code) {
                        if (ev.state != 0) {
                            if (!b->is_down) {
                                b->just_pressed = true;
                            }
                            b->is_down = true;
                            b->press_count++;
                        } else {
                            if (b->is_down) {
                                b->just_released = true;
                            }
                            b->is_down = false;
                        }
                    }
                }
            }
        } else if (ev.type == KHR_INPUT_MOTION) {
            map->mouse_x  = (uint32_t)ev.x;
            map->mouse_y  = (uint32_t)ev.y;
            map->mouse_dx += ev.dx;
            map->mouse_dy += ev.dy;

            for (uint32_t x_idx = 0; x_idx < KHR_AXIS_MAX; x_idx++) {
                khr_axis_binding_t* ax = &map->axes[x_idx];
                if (ax->source_type == KHR_AXIS_SRC_MOUSE_DX) {
                    ax->accum_raw += (float)ev.dx;
                } else if (ax->source_type == KHR_AXIS_SRC_MOUSE_DY) {
                    ax->accum_raw += (float)ev.dy;
                }
            }
        } else if (ev.type == KHR_INPUT_AXIS) {
            for (uint32_t x_idx = 0; x_idx < KHR_AXIS_MAX; x_idx++) {
                khr_axis_binding_t* ax = &map->axes[x_idx];
                if (ax->source_type == KHR_AXIS_SRC_MOUSE_WHEEL) {
                    ax->accum_raw += (float)ev.val_i32;
                }
            }
        }
    }

    /* 3. Compute analog axis outputs with deadzone, response curve, and temporal smoothing */
    for (uint32_t i = 0; i < KHR_AXIS_MAX; i++) {
        khr_axis_binding_t* ax = &map->axes[i];
        if (ax->source_type == KHR_AXIS_SRC_NONE) {
            continue;
        }

        float raw = 0.0f;
        if (ax->source_type == KHR_AXIS_SRC_DIGITAL_KEYS) {
            bool pos_down = false;
            bool neg_down = false;
            /* Direct raw keycode lookup */
            if (ax->pos_code < KHR_MAX_KEY_CODES && map->keys_down[ax->pos_code]) {
                pos_down = true;
            }
            if (ax->neg_code < KHR_MAX_KEY_CODES && map->keys_down[ax->neg_code]) {
                neg_down = true;
            }
            /* Also check actions for compatibility */
            for (uint32_t a = 0; a < KHR_ACTION_MAX; a++) {
                khr_action_binding_t* b = &map->actions[a];
                for (uint32_t k = 0; k < b->binding_count; k++) {
                    if (b->input_types[k] == KHR_INPUT_KEY) {
                        if (b->input_codes[k] == ax->pos_code && b->is_down) pos_down = true;
                        if (b->input_codes[k] == ax->neg_code && b->is_down) neg_down = true;
                    }
                }
            }
            raw = (pos_down ? 1.0f : 0.0f) - (neg_down ? 1.0f : 0.0f);
            raw *= ax->scale;
        } else {
            raw = ax->accum_raw * ax->scale;
        }

        /* Deadzone Processing */
        if (ax->deadzone > 0.0f) {
            float mag = fabsf(raw);
            if (mag <= ax->deadzone) {
                raw = 0.0f;
            } else {
                float sign = (raw > 0.0f) ? 1.0f : -1.0f;
                raw = sign * ((mag - ax->deadzone) / (1.0f - ax->deadzone));
            }
        }

        /* Non-linear Response Curve */
        if (ax->curve_exp > 1.0f && fabsf(raw) > 1e-6f) {
            float sign = (raw > 0.0f) ? 1.0f : -1.0f;
            raw = sign * powf(fabsf(raw), ax->curve_exp);
        }

        /* Clamping */
        if (raw < ax->min_clamp) raw = ax->min_clamp;
        if (raw > ax->max_clamp) raw = ax->max_clamp;

        /* Temporal Low-pass Filter Smoothing:
         * val_t = val_(t-1) + alpha * (target - val_(t-1)) */
        float alpha = ax->smoothing;
        ax->current_val = ax->previous_val + alpha * (raw - ax->previous_val);
    }
}

[[nodiscard]]
float khr_action_map_sample_axis(const khr_action_map_t* map, uint32_t axis_id, float alpha) {
    if (map == nullptr || axis_id >= KHR_AXIS_MAX) {
        return 0.0f;
    }
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;

    const khr_axis_binding_t* ax = &map->axes[axis_id];
    return (1.0f - alpha) * ax->previous_val + alpha * ax->current_val;
}

[[nodiscard]]
bool khr_action_map_is_down(const khr_action_map_t* map, uint32_t action_id) {
    if (map == nullptr || action_id >= KHR_ACTION_MAX) {
        return false;
    }
    return map->actions[action_id].is_down;
}

[[nodiscard]]
bool khr_action_map_just_pressed(const khr_action_map_t* map, uint32_t action_id) {
    if (map == nullptr || action_id >= KHR_ACTION_MAX) {
        return false;
    }
    return map->actions[action_id].just_pressed;
}

[[nodiscard]]
bool khr_action_map_just_released(const khr_action_map_t* map, uint32_t action_id) {
    if (map == nullptr || action_id >= KHR_ACTION_MAX) {
        return false;
    }
    return map->actions[action_id].just_released;
}

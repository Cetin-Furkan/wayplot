#ifndef KHOROS_CORE_INPUT_H
#define KHOROS_CORE_INPUT_H

#if __STDC_VERSION__ < 202311L
#error "Khoros requires pure ISO C23 (compile with -std=c23)"
#endif

#ifdef _STDBOOL_H
#error "<stdbool.h> is strictly banned. In C23, bool, true, and false are native language keywords."
#endif

#ifdef _STDALIGN_H
#error "<stdalign.h> is strictly banned. In C23, alignas and alignof are native language keywords."
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include "khoros/core/attributes.h"

constexpr uint32_t KHR_INPUT_RING_CAP = 256; /* Power of two for SPSC mask indexing */
constexpr uint32_t KHR_ACTION_MAX      = 64;
constexpr uint32_t KHR_AXIS_MAX        = 16;
constexpr uint32_t KHR_BINDINGS_PER_ACTION = 4;

typedef enum {
    KHR_INPUT_NONE = 0,
    KHR_INPUT_KEY,
    KHR_INPUT_BUTTON,
    KHR_INPUT_MOTION,
    KHR_INPUT_AXIS,
} khr_input_event_type_t;

typedef struct {
    uint32_t type;         /* khr_input_event_type_t */
    uint32_t code;         /* Key scancode, mouse button, or axis index */
    uint32_t state;        /* 1 = pressed, 0 = released */
    int32_t  x;            /* Absolute pointer x (fixed 24.8 or px) */
    int32_t  y;            /* Absolute pointer y (fixed 24.8 or px) */
    int32_t  dx;           /* Relative motion delta x */
    int32_t  dy;           /* Relative motion delta y */
    int32_t  val_i32;      /* Wheel value120 or discrete step */
    float    val_f32;      /* Continuous axis value */
    uint64_t timestamp_ns; /* Monotonic arrival timestamp */
} khr_input_event_t;

constexpr uint32_t KHR_MAX_KEY_CODES       = 512;
constexpr uint32_t KHR_MAX_BUTTON_CODES    = 32;

/* Lock-free Single-Producer Single-Consumer (SPSC) Input Ring Buffer */
typedef struct khr_input_ring {
    _Atomic uint32_t  head;
    uint8_t           _pad0[60]; /* Cache-line separation to eliminate false sharing */
    _Atomic uint32_t  tail;
    uint8_t           _pad1[60];
    _Atomic uint32_t  overflow_count;
    uint8_t           _pad2[60];
    khr_input_event_t events[KHR_INPUT_RING_CAP];
} khr_input_ring_t;

/* Digital Action Definition */
typedef struct {
    uint32_t input_types[KHR_BINDINGS_PER_ACTION];
    uint32_t input_codes[KHR_BINDINGS_PER_ACTION];
    uint32_t binding_count;
    bool     is_down;
    bool     just_pressed;
    bool     just_released;
    uint32_t press_count;
} khr_action_binding_t;

/* Analog Axis Source Type */
typedef enum {
    KHR_AXIS_SRC_NONE = 0,
    KHR_AXIS_SRC_DIGITAL_KEYS, /* Positive key + Negative key */
    KHR_AXIS_SRC_MOUSE_DX,     /* Mouse relative dx */
    KHR_AXIS_SRC_MOUSE_DY,     /* Mouse relative dy */
    KHR_AXIS_SRC_MOUSE_WHEEL,  /* Mouse scroll wheel */
} khr_axis_source_t;

/* Analog Axis Definition */
typedef struct {
    uint32_t source_type;  /* khr_axis_source_t */
    uint32_t pos_code;     /* Positive key or button */
    uint32_t neg_code;     /* Negative key or button */
    float    scale;        /* Sensitivity multiplier */
    float    deadzone;     /* Deadzone threshold [0.0, 1.0) */
    float    curve_exp;    /* Response curve exponent (1.0 = linear, 2.0 = quadratic) */
    float    smoothing;    /* Low-pass filter factor alpha in (0.0, 1.0] */
    float    min_clamp;    /* Min clamped value */
    float    max_clamp;    /* Max clamped value */

    float    current_val;  /* State at tick N */
    float    previous_val; /* State at tick N-1 (for render interpolation) */
    float    accum_raw;    /* Accumulated raw delta within tick window */
} khr_axis_binding_t;

/* Decoupled Action Mapper */
typedef struct {
    khr_action_binding_t actions[KHR_ACTION_MAX];
    khr_axis_binding_t   axes[KHR_AXIS_MAX];
    bool                 keys_down[KHR_MAX_KEY_CODES];
    bool                 buttons_down[KHR_MAX_BUTTON_CODES];
    uint32_t             mouse_x;
    uint32_t             mouse_y;
    int32_t              mouse_dx;
    int32_t              mouse_dy;
} khr_action_map_t;

/* Ring Buffer Functions */
void khr_input_ring_init(khr_input_ring_t* ring);

[[nodiscard]]
bool khr_input_ring_push(khr_input_ring_t* ring, const khr_input_event_t* ev);

[[nodiscard]]
bool khr_input_ring_pop(khr_input_ring_t* ring, khr_input_event_t* out_ev);

uint32_t khr_input_ring_drain(khr_input_ring_t* ring, khr_input_event_t* out_events, uint32_t max_count);

[[nodiscard]]
uint32_t khr_input_ring_overflow_count(const khr_input_ring_t* ring);

/* Action Mapping Functions */
void khr_action_map_init(khr_action_map_t* map);

void khr_action_map_bind_digital(khr_action_map_t* map, uint32_t action_id,
                                 uint32_t input_type, uint32_t input_code);

void khr_action_map_bind_axis_digital(khr_action_map_t* map, uint32_t axis_id,
                                      uint32_t pos_key, uint32_t neg_key, float scale);

void khr_action_map_bind_axis_mouse(khr_action_map_t* map, uint32_t axis_id,
                                    uint32_t mouse_source, float sensitivity);

void khr_action_map_set_axis_deadzone(khr_action_map_t* map, uint32_t axis_id,
                                      float deadzone, float curve_exp);

void khr_action_map_set_axis_smoothing(khr_action_map_t* map, uint32_t axis_id,
                                       float smoothing_factor);

void khr_action_map_set_axis_clamp(khr_action_map_t* map, uint32_t axis_id,
                                   float min_val, float max_val);

/* Fixed-tick update: drains ring buffer, processes edge transitions and analog curves */
void khr_action_map_tick(khr_action_map_t* map, khr_input_ring_t* ring, float dt);

/* Sub-tick query for render thread: alpha in [0.0, 1.0] */
[[nodiscard]]
float khr_action_map_sample_axis(const khr_action_map_t* map, uint32_t axis_id, float alpha);

[[nodiscard]]
bool khr_action_map_is_down(const khr_action_map_t* map, uint32_t action_id);

[[nodiscard]]
bool khr_action_map_just_pressed(const khr_action_map_t* map, uint32_t action_id);

[[nodiscard]]
bool khr_action_map_just_released(const khr_action_map_t* map, uint32_t action_id);

#endif /* KHOROS_CORE_INPUT_H */

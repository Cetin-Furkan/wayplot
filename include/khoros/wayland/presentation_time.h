#ifndef KHOROS_WAYLAND_PRESENTATION_TIME_H
#define KHOROS_WAYLAND_PRESENTATION_TIME_H

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
#include "khoros/core/attributes.h"
#include "khoros/wayland/client.h"
#include "khoros/wayland/wire.h"

/*
 * Wayland Presentation Time Protocol (wp_presentation_time).
 * Enables precise hardware presentation timestamps, display refresh period
 * extraction, and sub-millisecond frame pacing jitter compensation.
 */

/* Protocol constants */
#define KHR_WP_PRESENTATION_INTERFACE "wp_presentation"
constexpr uint32_t KHR_WP_PRESENTATION_VERSION = 1;

/* wp_presentation request opcodes */
constexpr uint16_t KHR_WP_PRESENTATION_DESTROY  = 0;
constexpr uint16_t KHR_WP_PRESENTATION_FEEDBACK = 1;

/* wp_presentation event opcodes */
constexpr uint16_t KHR_WP_PRESENTATION_EVENT_CLOCK_ID = 0;

/* wp_presentation_feedback event opcodes */
constexpr uint16_t KHR_WP_PRESENTATION_FEEDBACK_EVENT_SYNC_OUTPUT = 0;
constexpr uint16_t KHR_WP_PRESENTATION_FEEDBACK_EVENT_PRESENTED   = 1;
constexpr uint16_t KHR_WP_PRESENTATION_FEEDBACK_EVENT_DISCARDED   = 2;

/* wp_presentation_feedback.presented kind flags */
constexpr uint32_t KHR_WP_PRESENTATION_FEEDBACK_KIND_VSYNC         = 0x1;
constexpr uint32_t KHR_WP_PRESENTATION_FEEDBACK_KIND_HW_CLOCK      = 0x2;
constexpr uint32_t KHR_WP_PRESENTATION_FEEDBACK_KIND_HW_COMPLETION = 0x4;
constexpr uint32_t KHR_WP_PRESENTATION_FEEDBACK_KIND_ZERO_COPY     = 0x8;

constexpr size_t KHR_PRESENTATION_MAX_PENDING = 16;

/*
 * Feedback sample recorded upon receipt of wp_presentation_feedback.presented
 */
typedef struct {
    uint64_t presentation_time_ns; /* Hardware presentation timestamp (CLOCK_MONOTONIC) */
    uint64_t sequence;             /* Hardware V-sync refresh counter / presentation sequence */
    uint32_t refresh_ns;           /* Display refresh cycle period in nanoseconds */
    uint32_t flags;                /* Hardware presentation flags (KIND_VSYNC, HW_CLOCK, etc.) */
    uint64_t commit_seq;           /* Client-side frame commit index */
    uint64_t latency_ns;           /* Elapsed duration between commit and display presentation */
    int64_t  jitter_ns;            /* Frame delta deviation from display refresh period */
    bool     presented;            /* true if presented, false if discarded */
} khr_presentation_feedback_sample_t;

typedef struct {
    uint32_t feedback_id;
    uint64_t commit_seq;
    uint64_t commit_time_ns;
    bool     pending;
} khr_presentation_request_t;

/*
 * Presentation Time Manager & Frame Pacer
 */
typedef struct {
    khr_wl_client_t* client;
    uint32_t         wp_presentation_id;
    uint32_t         clock_id;
    bool             bound;

    /* In-flight feedback tracking */
    khr_presentation_request_t pending[KHR_PRESENTATION_MAX_PENDING];
    uint32_t                   pending_count;

    /* Metrics & Pacing */
    uint64_t last_presentation_time_ns;
    uint64_t last_sequence;
    uint32_t refresh_ns;           /* Detected display refresh rate (e.g. 16'666'666 ns = 60Hz) */
    uint32_t flags;
    uint64_t total_presented;
    uint64_t total_discarded;
    uint64_t last_latency_ns;
    int64_t  last_jitter_ns;

    /* Last received sample */
    khr_presentation_feedback_sample_t last_sample;
    bool                               has_sample;
} khr_presentation_time_t;

/*
 * Discover and bind wp_presentation global from Wayland registry.
 * Returns true if bound, false if not advertised or binding failed.
 */
[[nodiscard]]
bool khr_presentation_time_bind(khr_presentation_time_t* pt, khr_wl_client_t* client);

/*
 * Destroy wp_presentation object and release tracking resources.
 */
void khr_presentation_time_destroy(khr_presentation_time_t* pt);

/*
 * Request presentation feedback for a committed surface frame.
 */
[[nodiscard]]
bool khr_presentation_request_feedback(khr_presentation_time_t* pt,
                                       uint32_t surface_id,
                                       uint64_t commit_seq,
                                       uint32_t* out_feedback_id);

/*
 * Consume and process Wayland wire protocol events for presentation feedback.
 * Returns number of feedback events processed (presented or discarded).
 */
uint32_t khr_presentation_time_consume(khr_presentation_time_t* pt,
                                       const uint8_t* data,
                                       size_t len);

/*
 * Predict timestamp of next hardware V-sync presentation deadline.
 * Returns true if display refresh timing is locked, false if waiting for feedback.
 */
[[nodiscard]]
bool khr_presentation_predict_next_vsync(const khr_presentation_time_t* pt,
                                         uint64_t now_ns,
                                         uint64_t* out_next_vsync_ns,
                                         uint32_t* out_refresh_ns);

#endif /* KHOROS_WAYLAND_PRESENTATION_TIME_H */

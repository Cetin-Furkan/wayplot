#ifndef KHOROS_CORE_TIMESERIES_H
#define KHOROS_CORE_TIMESERIES_H

#if __STDC_VERSION__ < 202311L
#error "Khoros requires pure ISO C23 (compile with -std=c23)"
#endif

#ifdef _STDBOOL_H
#error "<stdbool.h> is strictly banned. In C23, bool, true, and false are native language keywords."
#endif

#ifdef _STDALIGN_H
#error "<stdalign.h> is strictly banned. In C23, alignas and alignof are native language keywords."
#endif

#include "khoros/core/attributes.h"
#include "khoros/core/physics.h"
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/*
 * Khoros Time Series Logger (Version 0.3 Named Contract B)
 *
 * Emits per-tick simulation state:
 *   tick,t,body_id,x,y,z,qx,qy,qz,qw,vx,vy,vz,wx,wy,wz,E,ncontacts
 */

typedef struct {
    FILE*    file;
    uint64_t ticks_written;
    float    dt;
    float    gravity;
} khr_timeseries_t;

[[nodiscard]]
bool khr_timeseries_open(khr_timeseries_t* ts,
                         const char* filepath,
                         float dt,
                         const float gravity[3],
                         const char* host_str);

void khr_timeseries_write_tick(khr_timeseries_t* ts,
                               uint64_t tick,
                               float t,
                               const khr_physics_world_t* world);

void khr_timeseries_close(khr_timeseries_t* ts);

/*
 * Khoros Frame Record Logger (Version 0.3 Named Contract C)
 *
 * Emits per-present presentation-time telemetry:
 *   frame,tick,presented_ns,monotonic_ns,draw_count,contacts,arena_used,status
 */

typedef struct {
    FILE*    file;
    uint64_t frames_written;
} khr_frame_record_t;

[[nodiscard]]
bool khr_frame_record_open(khr_frame_record_t* fr, const char* filepath);

void khr_frame_record_write(khr_frame_record_t* fr,
                            uint64_t frame,
                            uint64_t tick,
                            uint64_t presented_ns,
                            uint64_t monotonic_ns,
                            uint32_t draw_count,
                            uint32_t contacts,
                            size_t arena_used,
                            const char* status);

void khr_frame_record_close(khr_frame_record_t* fr);

/*
 * Deterministic State Hashing (Version 0.3 Named Contract E)
 *
 * Computes a 64-bit bitwise hash of all active rigid body state (position,
 * rotation quaternion, velocity, angular velocity) to guarantee reproducible runs.
 */
[[nodiscard]]
uint64_t khr_physics_world_hash_state(const khr_physics_world_t* world);

#endif /* KHOROS_CORE_TIMESERIES_H */

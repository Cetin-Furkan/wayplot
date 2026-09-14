#ifndef KHOROS_CORE_DECK_H
#define KHOROS_CORE_DECK_H

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
#include "khoros/gfx/scene.h"
#include <stdint.h>
#include <stddef.h>

/*
 * Khoros Experiment Deck Specification (# khoros-run v1)
 *
 * Example deck syntax:
 *   # khoros-run v1
 *   dt_s 0.008333333
 *   gravity 0 -9.80665 0
 *   floor_y -2.5
 *   seed 1
 *   ticks 1000
 *   body id=0 shape=sphere r=0.5 mass=2 e=0.7 mu=0.4 x=0 y=4 z=0 vx=0 vy=0 vz=0
 */

typedef struct khr_deck {
    float    dt_s;
    float    gravity[3];
    float    floor_y;
    uint32_t seed;
    uint32_t ticks;
    char     integrator[32];
    char     log_csv[256];

    khr_rigid_body_t bodies[KHR_PHYSICS_MAX_BODIES];
    uint32_t         body_count;
} khr_deck_t;

void khr_deck_init_defaults(khr_deck_t* deck);

[[nodiscard]]
bool khr_deck_parse_string(khr_deck_t* deck, const char* text, size_t len);

[[nodiscard]]
bool khr_deck_load_file(khr_deck_t* deck, const char* filepath);

[[nodiscard]]
bool khr_deck_apply_to_world(const khr_deck_t* deck, khr_physics_world_t* world);

[[nodiscard]]
uint32_t khr_deck_populate_instances(const khr_deck_t* deck,
                                     khr_gpu_instance_t* out_instances,
                                     uint32_t max_instances);

#endif /* KHOROS_CORE_DECK_H */

#include "test_framework.h"
#include "khoros/core/deck.h"
#include "khoros/core/timeseries.h"

[[nodiscard]]
bool test_simulation_determinism_hash(void) {
    khr_deck_t deck = {};
    TEST_ASSERT(khr_deck_load_file(&deck, "experiments/default_ballistic.deck"), "load experiment deck");

    /* Run 1: 1,000 ticks */
    khr_physics_world_t world1 = {};
    TEST_ASSERT(khr_deck_apply_to_world(&deck, &world1), "apply deck to world 1");
    for (uint32_t t = 0; t < 1000; t++) {
        khr_physics_world_step(&world1, deck.dt_s);
    }
    uint64_t hash1 = khr_physics_world_hash_state(&world1);
    TEST_ASSERT(hash1 != 0ULL, "hash 1 non-zero");

    /* Run 2: Exactly same 1,000 ticks */
    khr_physics_world_t world2 = {};
    TEST_ASSERT(khr_deck_apply_to_world(&deck, &world2), "apply deck to world 2");
    for (uint32_t t = 0; t < 1000; t++) {
        khr_physics_world_step(&world2, deck.dt_s);
    }
    uint64_t hash2 = khr_physics_world_hash_state(&world2);

    /* Assert bit-for-bit determinism contract */
    TEST_ASSERT_EQ(hash1, hash2, "consecutive runs must produce bit-for-bit identical 64-bit state hashes");

    /* Run 3: Perturbed initial position */
    khr_physics_world_t world3 = {};
    TEST_ASSERT(khr_deck_apply_to_world(&deck, &world3), "apply deck to world 3");
    world3.bodies[1].position[0] += 0.05f; /* Small perturbation */
    for (uint32_t t = 0; t < 1000; t++) {
        khr_physics_world_step(&world3, deck.dt_s);
    }
    uint64_t hash3 = khr_physics_world_hash_state(&world3);
    TEST_ASSERT(hash1 != hash3, "perturbed state must yield distinct hash");

    return true;
}

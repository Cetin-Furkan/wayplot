#include "test_framework.h"
#include "khoros/core/deck.h"
#include <string.h>
#include <math.h>

[[nodiscard]]
bool test_deck_parser_syntax_and_directives(void) {
    const char deck_text[] =
        "# khoros-run v1\n"
        "# Custom experiment deck\n"
        "dt_s 0.008333333\n"
        "gravity 0.0 -9.80665 0.0\n"
        "floor_y -2.5\n"
        "seed 1337\n"
        "ticks 500\n"
        "integrator symplectic_euler\n"
        "body id=0 shape=sphere r=0.75 mass=2.5 e=0.85 mu=0.35 x=1.0 y=4.0 z=-2.0 vx=0.5 vy=-1.0 vz=0.0\n"
        "body id=1 shape=aabb hx=0.5 hy=0.5 hz=0.5 mass=3.0 e=0.6 mu=0.5 x=0.0 y=8.0 z=0.0 vx=0.0 vy=0.0 vz=0.0\n";

    khr_deck_t deck = {};
    TEST_ASSERT(khr_deck_parse_string(&deck, deck_text, sizeof(deck_text) - 1), "parse valid deck string");

    TEST_ASSERT(fabsf(deck.dt_s - 0.008333333f) < 1e-6f, "parsed dt_s");
    TEST_ASSERT(fabsf(deck.gravity[0] - 0.0f) < 1e-6f, "parsed gravity x");
    TEST_ASSERT(fabsf(deck.gravity[1] - (-9.80665f)) < 1e-4f, "parsed gravity y");
    TEST_ASSERT(fabsf(deck.gravity[2] - 0.0f) < 1e-6f, "parsed gravity z");
    TEST_ASSERT(fabsf(deck.floor_y - (-2.5f)) < 1e-6f, "parsed floor_y");
    TEST_ASSERT_EQ(deck.seed, 1337U, "parsed seed");
    TEST_ASSERT_EQ(deck.ticks, 500U, "parsed ticks");
    TEST_ASSERT_EQ(strcmp(deck.integrator, "symplectic_euler"), 0, "parsed integrator name");
    TEST_ASSERT_EQ(deck.body_count, 2U, "parsed body count");

    /* Verify Body 0 */
    const khr_rigid_body_t* b0 = &deck.bodies[0];
    TEST_ASSERT_EQ(b0->user_id, 0U, "b0 id");
    TEST_ASSERT_EQ((int)b0->shape.type, (int)KHR_SHAPE_SPHERE, "b0 shape sphere");
    TEST_ASSERT(fabsf(b0->shape.sphere.radius - 0.75f) < 1e-6f, "b0 radius");
    TEST_ASSERT(fabsf(b0->mass - 2.5f) < 1e-6f, "b0 mass");
    TEST_ASSERT(fabsf(b0->restitution - 0.85f) < 1e-6f, "b0 restitution");
    TEST_ASSERT(fabsf(b0->friction - 0.35f) < 1e-6f, "b0 friction");
    TEST_ASSERT(fabsf(b0->position[0] - 1.0f) < 1e-6f, "b0 x");
    TEST_ASSERT(fabsf(b0->position[1] - 4.0f) < 1e-6f, "b0 y");
    TEST_ASSERT(fabsf(b0->position[2] - (-2.0f)) < 1e-6f, "b0 z");
    TEST_ASSERT(fabsf(b0->velocity[0] - 0.5f) < 1e-6f, "b0 vx");
    TEST_ASSERT(fabsf(b0->velocity[1] - (-1.0f)) < 1e-6f, "b0 vy");
    TEST_ASSERT(fabsf(b0->velocity[2] - 0.0f) < 1e-6f, "b0 vz");

    /* Verify Body 1 */
    const khr_rigid_body_t* b1 = &deck.bodies[1];
    TEST_ASSERT_EQ(b1->user_id, 1U, "b1 id");
    TEST_ASSERT_EQ((int)b1->shape.type, (int)KHR_SHAPE_AABB, "b1 shape aabb");
    TEST_ASSERT(fabsf(b1->shape.aabb.half_extents[0] - 0.5f) < 1e-6f, "b1 hx");
    TEST_ASSERT(fabsf(b1->mass - 3.0f) < 1e-6f, "b1 mass");

    /* Test applying to physics world */
    khr_physics_world_t world = {};
    TEST_ASSERT(khr_deck_apply_to_world(&deck, &world), "apply to world");
    /* World has static floor + 2 dynamic bodies = 3 total bodies */
    TEST_ASSERT_EQ(world.body_count, 3U, "world body count with floor");
    TEST_ASSERT(fabsf(world.gravity[1] - (-9.80665f)) < 1e-4f, "world gravity y");

    /* Test populating instances */
    khr_gpu_instance_t instances[16] = {};
    uint32_t inst_count = khr_deck_populate_instances(&deck, instances, 16);
    TEST_ASSERT_EQ(inst_count, 2U, "populate dynamic instances count");
    TEST_ASSERT(fabsf(instances[0].position[1] - 4.0f) < 1e-6f, "inst0 y position");
    TEST_ASSERT(fabsf(instances[0].radius - 0.75f) < 1e-6f, "inst0 radius");

    return true;
}

[[nodiscard]]
bool test_deck_file_loading(void) {
    khr_deck_t deck = {};
    TEST_ASSERT(khr_deck_load_file(&deck, "experiments/default_ballistic.deck"), "load default deck file");
    TEST_ASSERT_EQ(deck.body_count, 3U, "default deck 3 bodies");
    TEST_ASSERT_EQ(deck.seed, 1U, "default deck seed 1");
    TEST_ASSERT(fabsf(deck.floor_y - (-2.5f)) < 1e-6f, "default deck floor_y");

    /* Test non-existent file */
    khr_deck_t missing_deck = {};
    TEST_ASSERT(!khr_deck_load_file(&missing_deck, "experiments/does_not_exist.deck"), "missing file fails gracefully");

    return true;
}

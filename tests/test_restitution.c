#include "test_framework.h"
#include "khoros/core/physics.h"
#include <math.h>
#include <stdio.h>

[[nodiscard]]
bool test_restitution_normal_impact_map(void) {
    const float test_e[] = { 0.0f, 0.25f, 0.50f, 0.75f, 0.90f };
    constexpr size_t num_tests = sizeof(test_e) / sizeof(test_e[0]);
    const float radius = 0.5f;
    const float v_in = -6.0f; /* 6 m/s downward impact velocity */

    printf("    --- Measured Restitution Map (Sphere vs Static Plane) ---\n");

    for (size_t k = 0; k < num_tests; k++) {
        float target_e = test_e[k];

        khr_physics_world_t world = {};
        khr_physics_world_init(&world);
        /* Zero out gravity during contact impulse evaluation to isolate restitution equation */
        world.gravity[0] = 0.0f;
        world.gravity[1] = 0.0f;
        world.gravity[2] = 0.0f;

        /* Static Floor plane at KHR_PHYSICS_FLOOR_Y with perfect restitution */
        khr_rigid_body_t floor_body;
        float floor_n[3] = { 0.0f, 1.0f, 0.0f };
        khr_rigid_body_init_plane(&floor_body, floor_n, KHR_PHYSICS_FLOOR_Y, 1.0f, 0.0f);
        (void)khr_physics_world_add_body(&world, &floor_body);

        /* Dynamic Sphere positioned in contact with the plane at impact onset */
        float init_pos[3] = { 0.0f, KHR_PHYSICS_FLOOR_Y + radius - 0.005f, 0.0f };
        khr_rigid_body_t sphere;
        khr_rigid_body_init_sphere(&sphere, init_pos, radius, 2.0f, target_e, 0.0f);
        sphere.velocity[1] = v_in;
        (void)khr_physics_world_add_body(&world, &sphere);

        /* Step simulation */
        constexpr float dt = 1.0f / 60.0f;
        khr_physics_world_step(&world, dt);

        float v_post = world.bodies[1].velocity[1];
        float measured_e = -v_post / v_in;

        printf("    [Restitution] target e=%0.2f -> post_vn=%+0.3f m/s (measured e=%0.3f)\n",
               target_e, v_post, measured_e);

        /* Assertion: v_n^+ + e * v_n^- approx 0 */
        float eq_residual = fabsf(v_post + target_e * v_in);
        TEST_ASSERT(eq_residual <= 0.35f, "Post-contact velocity must satisfy v_n^+ + e*v_n^- ~ 0");

        if (target_e > 0.05f) {
            TEST_ASSERT(v_post > 0.0f, "Rebound velocity must be positive for non-zero restitution");
        }
    }

    return true;
}

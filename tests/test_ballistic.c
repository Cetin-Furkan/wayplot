#include "test_framework.h"
#include "khoros/core/physics.h"
#include <math.h>
#include <stdio.h>

[[nodiscard]]
bool test_ballistic_symplectic_convergence(void) {
    /* Initial state for Ballistic IVP */
    const float x0[3] = { 1.5f, 20.0f, -2.0f };
    const float v0[3] = { 4.0f, 15.0f, -3.0f };
    const float T = 1.0f; /* 1 second integration duration */

    /* Step sizes to evaluate convergence order: dt, dt/2, dt/4 */
    const float dt_arr[3] = { 1.0f / 30.0f, 1.0f / 60.0f, 1.0f / 120.0f };
    float err_max[3] = { 0.0f, 0.0f, 0.0f };

    for (int k = 0; k < 3; k++) {
        float dt = dt_arr[k];
        uint32_t steps = (uint32_t)roundf(T / dt);

        khr_physics_world_t world = {};
        khr_physics_world_init(&world);
        /* Ensure no collisions occur during free-fall IVP */
        world.body_count = 0;

        khr_rigid_body_t body;
        khr_rigid_body_init_sphere(&body, x0, 0.5f, 2.0f, 0.7f, 0.4f);
        body.velocity[0] = v0[0];
        body.velocity[1] = v0[1];
        body.velocity[2] = v0[2];
        (void)khr_physics_world_add_body(&world, &body);

        float max_e = 0.0f;
        for (uint32_t s = 1; s <= steps; s++) {
            khr_physics_world_step(&world, dt);
            float t = (float)s * dt;

            /* Exact analytical IVP solution: x(t) = x0 + v0*t + 0.5*g*t^2 */
            float x_exact = x0[0] + v0[0] * t + 0.5f * world.gravity[0] * t * t;
            float y_exact = x0[1] + v0[1] * t + 0.5f * world.gravity[1] * t * t;
            float z_exact = x0[2] + v0[2] * t + 0.5f * world.gravity[2] * t * t;

            float ex = fabsf(world.bodies[0].position[0] - x_exact);
            float ey = fabsf(world.bodies[0].position[1] - y_exact);
            float ez = fabsf(world.bodies[0].position[2] - z_exact);

            float e_step = fmaxf(ex, fmaxf(ey, ez));
            if (e_step > max_e) {
                max_e = e_step;
            }
        }
        err_max[k] = max_e;
    }

    /* Compute observed numerical convergence order */
    float order12 = logf(err_max[0] / err_max[1]) / logf(dt_arr[0] / dt_arr[1]);
    float order23 = logf(err_max[1] / err_max[2]) / logf(dt_arr[1] / dt_arr[2]);

    printf("    [Ballistic IVP] dt=1/30s e=%.5f m | dt=1/60s e=%.5f m | dt=1/120s e=%.5f m\n",
           err_max[0], err_max[1], err_max[2]);
    printf("    [Ballistic IVP] Observed order p12=%.3f, p23=%.3f (theoretical=1.000)\n",
           order12, order23);

    /* Convergence order must be >= 0.70 of theoretical 1st order symplectic Euler */
    TEST_ASSERT(order12 >= 0.70f, "Ballistic convergence order (p12) must be >= 0.70");
    TEST_ASSERT(order23 >= 0.70f, "Ballistic convergence order (p23) must be >= 0.70");

    /* Error must decrease monotonically with step size */
    TEST_ASSERT(err_max[1] < err_max[0], "Error must decrease when step size is halved");
    TEST_ASSERT(err_max[2] < err_max[1], "Error must decrease when step size is halved again");

    return true;
}

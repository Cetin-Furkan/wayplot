#include "khoros/core/spatial.h"
#include "khoros/core/physics.h"
#include "khoros/core/topology.h"
#include "khoros/gfx/gpu_math.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <assert.h>

#define ASSERT_TRUE(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "Assertion failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
        return false; \
    } \
} while(0)

#define ASSERT_FLOAT_NEAR(a, b, eps) do { \
    float diff = fabsf((a) - (b)); \
    if (diff > (eps)) { \
        fprintf(stderr, "Assertion failed: |%f - %f| = %f > %f (%s:%d)\n", \
                (float)(a), (float)(b), diff, (float)(eps), __FILE__, __LINE__); \
        return false; \
    } \
} while(0)

[[nodiscard]]
bool test_morton3d_encoding_and_ordering(void) {
    /* 1. Expansion bit-mask test: lowest 10 bits spread to 30 bits */
    uint32_t exp0 = khr_morton3d_expand(0x000);
    ASSERT_TRUE(exp0 == 0);

    uint32_t exp_one = khr_morton3d_expand(1);
    ASSERT_TRUE(exp_one == 1);

    uint32_t exp_all = khr_morton3d_expand(1023); /* 0x3FF */
    ASSERT_TRUE(exp_all == 0x09249249U);

    /* 2. Encoding inside bounding domain */
    khr_aabb_t domain = {
        .min = { -10.0f, -10.0f, -10.0f },
        .max = {  10.0f,  10.0f,  10.0f },
    };

    uint32_t code_min = khr_morton3d_encode(-10.0f, -10.0f, -10.0f, &domain);
    ASSERT_TRUE(code_min == 0);

    uint32_t code_max = khr_morton3d_encode(10.0f, 10.0f, 10.0f, &domain);
    ASSERT_TRUE(code_max == 0x3FFFFFFFU); /* 30 bits all 1s */

    /* Monotonicity along diagonal */
    uint32_t code_mid1 = khr_morton3d_encode(-5.0f, -5.0f, -5.0f, &domain);
    uint32_t code_mid2 = khr_morton3d_encode(0.0f, 0.0f, 0.0f, &domain);
    uint32_t code_mid3 = khr_morton3d_encode(5.0f, 5.0f, 5.0f, &domain);

    ASSERT_TRUE(code_min < code_mid1);
    ASSERT_TRUE(code_mid1 < code_mid2);
    ASSERT_TRUE(code_mid2 < code_mid3);
    ASSERT_TRUE(code_mid3 < code_max);

    return true;
}

[[nodiscard]]
bool test_radix_sort_morton_codes(void) {
    khr_spatial_item_t items[8] = {
        { .id = 0, .morton_code = 9999 },
        { .id = 1, .morton_code = 12 },
        { .id = 2, .morton_code = 543210 },
        { .id = 3, .morton_code = 4 },
        { .id = 4, .morton_code = 12345678 },
        { .id = 5, .morton_code = 12 }, /* Duplicate key */
        { .id = 6, .morton_code = 1 },
        { .id = 7, .morton_code = 98765 },
    };

    khr_radix_sort_morton(items, 8);

    /* Verify strictly sorted in non-decreasing order */
    for (uint32_t i = 1; i < 8; i++) {
        ASSERT_TRUE(items[i - 1].morton_code <= items[i].morton_code);
    }
    ASSERT_TRUE(items[0].morton_code == 1);
    ASSERT_TRUE(items[1].morton_code == 4);
    ASSERT_TRUE(items[2].morton_code == 12);
    ASSERT_TRUE(items[3].morton_code == 12);
    ASSERT_TRUE(items[7].morton_code == 12345678);

    return true;
}

[[nodiscard]]
bool test_lbvh_tree_construction_and_bounds(void) {
    khr_spatial_item_t items[4] = {
        { .id = 10, .bounds = { .min = { -5.0f, -1.0f, -1.0f }, .max = { -4.0f, 1.0f, 1.0f } } },
        { .id = 20, .bounds = { .min = { -2.0f, -1.0f, -1.0f }, .max = { -1.0f, 1.0f, 1.0f } } },
        { .id = 30, .bounds = { .min = {  1.0f, -1.0f, -1.0f }, .max = {  2.0f, 1.0f, 1.0f } } },
        { .id = 40, .bounds = { .min = {  4.0f, -1.0f, -1.0f }, .max = {  5.0f, 1.0f, 1.0f } } },
    };

    khr_lbvh_t bvh;
    bool ok = khr_lbvh_build(&bvh, items, 4);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(bvh.root >= 0);
    ASSERT_TRUE(bvh.leaf_count == 4);
    ASSERT_TRUE(bvh.node_count > 4);

    /* Root bounds must enclose all items: [-5, -1, -1] to [5, 1, 1] */
    ASSERT_FLOAT_NEAR(bvh.nodes[bvh.root].bounds.min[0], -5.0f, 1e-4f);
    ASSERT_FLOAT_NEAR(bvh.nodes[bvh.root].bounds.max[0],  5.0f, 1e-4f);
    ASSERT_FLOAT_NEAR(bvh.nodes[bvh.root].bounds.min[1], -1.0f, 1e-4f);
    ASSERT_FLOAT_NEAR(bvh.nodes[bvh.root].bounds.max[1],  1.0f, 1e-4f);

    /* All internal nodes must enclose their children */
    for (uint32_t i = 0; i < bvh.node_count; i++) {
        const khr_lbvh_node_t* n = &bvh.nodes[i];
        if (!n->is_leaf) {
            ASSERT_TRUE(n->left >= 0);
            ASSERT_TRUE(n->right >= 0);
            const khr_lbvh_node_t* l = &bvh.nodes[n->left];
            const khr_lbvh_node_t* r = &bvh.nodes[n->right];
            ASSERT_TRUE(n->bounds.min[0] <= l->bounds.min[0]);
            ASSERT_TRUE(n->bounds.max[0] >= l->bounds.max[0]);
            ASSERT_TRUE(n->bounds.min[0] <= r->bounds.min[0]);
            ASSERT_TRUE(n->bounds.max[0] >= r->bounds.max[0]);
        }
    }

    return true;
}

[[nodiscard]]
bool test_lbvh_broadphase_query(void) {
    khr_spatial_item_t items[4] = {
        { .id = 101, .bounds = { .min = { -10.0f, -1.0f, -1.0f }, .max = { -8.0f, 1.0f, 1.0f } } },
        { .id = 102, .bounds = { .min = {  -1.0f, -1.0f, -1.0f }, .max = {  1.0f, 1.0f, 1.0f } } },
        { .id = 103, .bounds = { .min = {   8.0f, -1.0f, -1.0f }, .max = { 10.0f, 1.0f, 1.0f } } },
        { .id = 104, .bounds = { .min = {  20.0f, -1.0f, -1.0f }, .max = { 22.0f, 1.0f, 1.0f } } },
    };

    khr_lbvh_t bvh;
    ASSERT_TRUE(khr_lbvh_build(&bvh, items, 4));

    /* Query AABB overlapping only item 102 */
    khr_aabb_t query_box = {
        .min = { -0.5f, -0.5f, -0.5f },
        .max = {  0.5f,  0.5f,  0.5f },
    };
    uint32_t results[8];
    uint32_t count = khr_lbvh_query_aabb(&bvh, &query_box, results, 8);
    ASSERT_TRUE(count == 1);
    ASSERT_TRUE(results[0] == 102);

    /* Query sphere overlapping items 101 and 102 */
    float center[3] = { -4.5f, 0.0f, 0.0f };
    float radius = 5.0f;
    count = khr_lbvh_query_sphere(&bvh, center, radius, results, 8);
    ASSERT_TRUE(count == 2);
    bool found101 = (results[0] == 101 || results[1] == 101);
    bool found102 = (results[0] == 102 || results[1] == 102);
    ASSERT_TRUE(found101 && found102);

    return true;
}

[[nodiscard]]
bool test_narrowphase_sphere_sphere_collision(void) {
    khr_rigid_body_t sA, sB;
    float posA[3] = { 0.0f, 0.0f, 0.0f };
    float posB[3] = { 1.5f, 0.0f, 0.0f };

    khr_rigid_body_init_sphere(&sA, posA, 1.0f, 1.0f, 0.5f, 0.3f);
    khr_rigid_body_init_sphere(&sB, posB, 1.0f, 1.0f, 0.5f, 0.3f);

    khr_contact_t c = {};
    bool hit = khr_collide_sphere_sphere(&sA, 0, &sB, 1, &c);
    ASSERT_TRUE(hit);
    ASSERT_TRUE(c.body_a == 0 && c.body_b == 1);
    ASSERT_FLOAT_NEAR(c.normal[0], 1.0f, 1e-4f);
    ASSERT_FLOAT_NEAR(c.normal[1], 0.0f, 1e-4f);
    ASSERT_FLOAT_NEAR(c.normal[2], 0.0f, 1e-4f);
    ASSERT_FLOAT_NEAR(c.penetration, 0.5f, 1e-4f); /* (1 + 1) - 1.5 = 0.5 */
    ASSERT_FLOAT_NEAR(c.point[0], 0.75f, 1e-4f);

    /* Separate spheres */
    sB.position[0] = 3.0f;
    hit = khr_collide_sphere_sphere(&sA, 0, &sB, 1, &c);
    ASSERT_TRUE(!hit);

    return true;
}

[[nodiscard]]
bool test_narrowphase_sphere_plane_collision(void) {
    khr_rigid_body_t plane, sphere;
    float n[3] = { 0.0f, 1.0f, 0.0f };
    khr_rigid_body_init_plane(&plane, n, 0.0f, 0.7f, 0.4f);

    /* Sphere at y = 0.6, radius 1.0 -> penetration = 0.4 */
    float pos[3] = { 0.0f, 0.6f, 0.0f };
    khr_rigid_body_init_sphere(&sphere, pos, 1.0f, 2.0f, 0.7f, 0.4f);

    khr_contact_t c = {};
    bool hit = khr_collide_sphere_plane(&sphere, 1, &plane, 0, &c);
    ASSERT_TRUE(hit);
    ASSERT_TRUE(c.body_a == 0 && c.body_b == 1);
    ASSERT_FLOAT_NEAR(c.normal[1], 1.0f, 1e-4f);
    ASSERT_FLOAT_NEAR(c.penetration, 0.4f, 1e-4f);
    ASSERT_FLOAT_NEAR(c.point[1], 0.0f, 1e-3f);

    /* Sphere at y = 1.2 -> no hit */
    sphere.position[1] = 1.2f;
    hit = khr_collide_sphere_plane(&sphere, 1, &plane, 0, &c);
    ASSERT_TRUE(!hit);

    return true;
}

[[nodiscard]]
bool test_narrowphase_sphere_aabb_collision(void) {
    khr_rigid_body_t box, sphere;
    float box_pos[3] = { 0.0f, 0.0f, 0.0f };
    float half_ext[3] = { 1.0f, 1.0f, 1.0f };
    khr_rigid_body_init_aabb(&box, box_pos, half_ext, 0.0f, 0.5f, 0.5f);

    /* Sphere touching +X face of the box */
    float sph_pos[3] = { 1.6f, 0.0f, 0.0f };
    khr_rigid_body_init_sphere(&sphere, sph_pos, 1.0f, 1.0f, 0.5f, 0.5f);

    khr_contact_t c = {};
    bool hit = khr_collide_sphere_aabb(&sphere, 1, &box, 0, &c);
    ASSERT_TRUE(hit);
    ASSERT_TRUE(c.body_a == 0 && c.body_b == 1);
    ASSERT_FLOAT_NEAR(c.normal[0], 1.0f, 1e-4f);
    ASSERT_FLOAT_NEAR(c.penetration, 0.4f, 1e-4f); /* 1.0 - (1.6 - 1.0) = 0.4 */
    ASSERT_FLOAT_NEAR(c.point[0], 1.0f, 1e-4f);

    return true;
}

[[nodiscard]]
bool test_narrowphase_sphere_capsule_collision(void) {
    khr_rigid_body_t cap, sphere;
    float p0[3] = { 0.0f, -2.0f, 0.0f };
    float p1[3] = { 0.0f,  2.0f, 0.0f };
    khr_rigid_body_init_capsule(&cap, p0, p1, 0.5f, 1.0f, 0.5f, 0.5f);

    /* Sphere along the cylinder body of the capsule */
    float sph_pos[3] = { 1.0f, 0.5f, 0.0f };
    khr_rigid_body_init_sphere(&sphere, sph_pos, 0.8f, 1.0f, 0.5f, 0.5f);

    khr_contact_t c = {};
    bool hit = khr_collide_sphere_capsule(&sphere, 1, &cap, 0, &c);
    ASSERT_TRUE(hit);
    ASSERT_TRUE(c.body_a == 0 && c.body_b == 1);
    ASSERT_FLOAT_NEAR(c.normal[0], 1.0f, 1e-4f);
    ASSERT_FLOAT_NEAR(c.penetration, 0.3f, 1e-4f); /* (0.5 + 0.8) - 1.0 = 0.3 */

    return true;
}

[[nodiscard]]
bool test_rigid_body_impulse_restitution_and_friction(void) {
    khr_physics_world_t world;
    khr_physics_world_init(&world);
    world.gravity[0] = 0.0f;
    world.gravity[1] = 0.0f;
    world.gravity[2] = 0.0f;

    /* Static floor at y = 0 */
    khr_rigid_body_t floor_body;
    float n[3] = { 0.0f, 1.0f, 0.0f };
    khr_rigid_body_init_plane(&floor_body, n, 0.0f, 0.8f, 0.5f);
    uint32_t id_floor = khr_physics_world_add_body(&world, &floor_body);
    ASSERT_TRUE(id_floor != UINT32_MAX);

    /* Dynamic sphere moving downward at -4.0 m/s with tangential velocity +2.0 m/s */
    khr_rigid_body_t ball;
    float bpos[3] = { 0.0f, 0.95f, 0.0f };
    khr_rigid_body_init_sphere(&ball, bpos, 1.0f, 2.0f, 0.8f, 0.5f);
    ball.velocity[0] = 2.0f;
    ball.velocity[1] = -4.0f;
    uint32_t id_ball = khr_physics_world_add_body(&world, &ball);
    ASSERT_TRUE(id_ball != UINT32_MAX);

    /* Step physics simulation once */
    khr_physics_world_step(&world, 0.016f);

    /* Normal velocity should bounce back upwards (positive vy) */
    ASSERT_TRUE(world.bodies[id_ball].velocity[1] > 0.0f);

    /* Tangential velocity should be reduced by Coulomb friction */
    ASSERT_TRUE(world.bodies[id_ball].velocity[0] < 2.0f);

    return true;
}

[[nodiscard]]
bool test_physics_world_fixed_tick_simulation_stability(void) {
    khr_physics_world_t world;
    khr_physics_world_init(&world);
    /* Standard Earth gravity */
    world.gravity[0] = 0.0f;
    world.gravity[1] = -9.81f;
    world.gravity[2] = 0.0f;

    /* Ground floor at y = 0 */
    khr_rigid_body_t floor_body;
    float n[3] = { 0.0f, 1.0f, 0.0f };
    khr_rigid_body_init_plane(&floor_body, n, 0.0f, 0.6f, 0.4f);
    (void)khr_physics_world_add_body(&world, &floor_body);

    /* Sphere dropped from y = 5.0m */
    khr_rigid_body_t ball;
    float bpos[3] = { 0.0f, 5.0f, 0.0f };
    khr_rigid_body_init_sphere(&ball, bpos, 1.0f, 1.0f, 0.6f, 0.4f);
    uint32_t id_ball = khr_physics_world_add_body(&world, &ball);

    /* Run 120 ticks at 120 Hz (1 second) */
    float dt = 1.0f / 120.0f;
    for (int t = 0; t < 120; t++) {
        khr_physics_world_step(&world, dt);
        /* The sphere center must never penetrate below the floor radius (y >= 1.0 - slop) */
        ASSERT_TRUE(world.bodies[id_ball].position[1] >= 0.95f);
    }

    /* Ball is in a stable bouncing/resting condition above floor */
    ASSERT_TRUE(world.bodies[id_ball].position[1] >= 0.95f);

    return true;
}

[[nodiscard]]
bool test_physics_double_buffer_bda_integration(void) {
    khr_topology_t topo = {};
    ASSERT_TRUE(khr_topology_init(&topo));

    /* Set up 2 instances in double-buffered slot arrays */
    khr_gpu_instance_t buf_a[2] = {
        { .position = { 0.0f, 4.0f, 0.0f }, .radius = 1.0f, .scale = { 1.0f, 1.0f, 1.0f } },
        { .position = { 2.5f, 4.0f, 0.0f }, .radius = 1.0f, .scale = { 1.0f, 1.0f, 1.0f } },
    };
    khr_gpu_instance_t buf_b[2] = {};

    ASSERT_TRUE(khr_topology_start_sim(&topo, 120, 2, buf_a, buf_b, 0x1000, 0x2000));
    khr_topology_sim_set_motion(&topo, true);

    /* Step the simulation for 10 ticks */
    for (uint64_t tick = 1; tick <= 10; tick++) {
        khr_topology_sim_step(&topo, tick);
    }

    uint64_t read_bda = 0, prev_bda = 0;
    float alpha = 0.0f;
    uint32_t flags = 0;
    khr_topology_sim_get_render_state(&topo, 0, &read_bda, &prev_bda, &alpha, &flags);

    /* Verify BDA slot addresses toggle and simulation state updated */
    ASSERT_TRUE(read_bda == 0x1000 || read_bda == 0x2000);
    ASSERT_TRUE(flags & 1U); /* Interpolation enabled */

    khr_topology_stop_sim(&topo);
    khr_topology_destroy(&topo);
    return true;
}

[[nodiscard]]
bool test_capsule_plane_collision(void) {
    khr_rigid_body_t plane = {};
    float n[3] = { 0.0f, 1.0f, 0.0f };
    khr_rigid_body_init_plane(&plane, n, -2.5f, 0.5f, 0.3f);

    khr_rigid_body_t capsule = {};
    float p0[3] = { 0.0f, -1.0f, 0.0f };
    float p1[3] = { 0.0f,  1.0f, 0.0f };
    khr_rigid_body_init_capsule(&capsule, p0, p1, 0.5f, 2.0f, 0.5f, 0.3f);
    capsule.position[0] = 0.0f;
    capsule.position[1] = -1.8f; /* lowest point = -1.8 - 1.0 - 0.5 = -3.3. Floor = -2.5. Pen = 0.8 */
    capsule.position[2] = 0.0f;

    khr_contact_t c = {};
    bool hit = khr_collide_capsule_plane(&capsule, 1, &plane, 0, &c);
    ASSERT_TRUE(hit);
    ASSERT_TRUE(c.normal[1] == 1.0f);
    ASSERT_FLOAT_NEAR(c.penetration, 0.8f, 1e-4f);
    ASSERT_FLOAT_NEAR(c.point[1], -2.5f, 1e-4f);

    /* Move capsule above floor: no collision */
    capsule.position[1] = 0.0f; /* lowest point = -1.5 > -2.5 */
    hit = khr_collide_capsule_plane(&capsule, 1, &plane, 0, &c);
    ASSERT_TRUE(!hit);

    return true;
}

[[nodiscard]]
bool test_aabb_plane_collision(void) {
    khr_rigid_body_t plane = {};
    float n[3] = { 0.0f, 1.0f, 0.0f };
    khr_rigid_body_init_plane(&plane, n, -2.5f, 0.4f, 0.5f);

    khr_rigid_body_t aabb = {};
    float hx[3] = { 0.8f, 0.8f, 0.8f };
    float pos[3] = { 0.0f, -2.0f, 0.0f }; /* bottom = -2.8. Floor = -2.5. Pen = 0.3 */
    khr_rigid_body_init_aabb(&aabb, pos, hx, 3.0f, 0.4f, 0.5f);

    khr_contact_t c = {};
    bool hit = khr_collide_aabb_plane(&aabb, 1, &plane, 0, &c);
    ASSERT_TRUE(hit);
    ASSERT_TRUE(c.normal[1] == 1.0f);
    ASSERT_FLOAT_NEAR(c.penetration, 0.3f, 1e-4f);

    return true;
}

[[nodiscard]]
bool test_capsule_capsule_collision(void) {
    /* 1. Two parallel capsules along Y, separated along X */
    khr_rigid_body_t cA = {}, cB = {};
    float p0[3] = { 0.0f, -1.0f, 0.0f };
    float p1[3] = { 0.0f,  1.0f, 0.0f };
    khr_rigid_body_init_capsule(&cA, p0, p1, 0.5f, 2.0f, 0.5f, 0.3f);
    khr_rigid_body_init_capsule(&cB, p0, p1, 0.5f, 2.0f, 0.5f, 0.3f);

    cA.position[0] = 0.0f; cA.position[1] = 0.0f; cA.position[2] = 0.0f;
    cB.position[0] = 0.8f; cB.position[1] = 0.0f; cB.position[2] = 0.0f;

    khr_contact_t c = {};
    bool hit = khr_collide_capsule_capsule(&cA, 0, &cB, 1, &c);
    ASSERT_TRUE(hit);
    ASSERT_TRUE(c.body_a == 0 && c.body_b == 1);
    ASSERT_FLOAT_NEAR(c.penetration, 0.2f, 1e-4f);
    ASSERT_FLOAT_NEAR(c.normal[0], 1.0f, 1e-4f);
    ASSERT_FLOAT_NEAR(c.normal[1], 0.0f, 1e-4f);
    ASSERT_FLOAT_NEAR(c.normal[2], 0.0f, 1e-4f);

    /* 2. Disjoint parallel capsules */
    cB.position[0] = 1.5f;
    hit = khr_collide_capsule_capsule(&cA, 0, &cB, 1, &c);
    ASSERT_TRUE(!hit);

    /* 3. Two crossed (perpendicular) capsules */
    float pB0[3] = { -1.0f, 0.0f, 0.0f };
    float pB1[3] = {  1.0f, 0.0f, 0.0f };
    khr_rigid_body_init_capsule(&cB, pB0, pB1, 0.5f, 2.0f, 0.5f, 0.3f);
    cB.position[0] = 0.0f; cB.position[1] = 0.0f; cB.position[2] = 0.7f;
    hit = khr_collide_capsule_capsule(&cA, 0, &cB, 1, &c);
    ASSERT_TRUE(hit);
    ASSERT_FLOAT_NEAR(c.penetration, 0.3f, 1e-4f);
    ASSERT_FLOAT_NEAR(c.normal[2], 1.0f, 1e-4f);

    return true;
}

[[nodiscard]]
bool test_capsule_aabb_collision(void) {
    khr_rigid_body_t capsule = {}, aabb = {};
    float p0[3] = { 0.0f, -0.5f, 0.0f };
    float p1[3] = { 0.0f,  0.5f, 0.0f };
    khr_rigid_body_init_capsule(&capsule, p0, p1, 0.4f, 2.0f, 0.5f, 0.3f);

    float hx[3] = { 1.0f, 1.0f, 1.0f };
    khr_rigid_body_init_aabb(&aabb, (float[]){ 0.0f, 0.0f, 0.0f }, hx, 3.0f, 0.5f, 0.3f);

    capsule.position[0] = 0.0f; capsule.position[1] = 1.6f; capsule.position[2] = 0.0f;

    khr_contact_t c = {};
    bool hit = khr_collide_capsule_aabb(&capsule, 0, &aabb, 1, &c);
    ASSERT_TRUE(hit);
    ASSERT_TRUE(c.body_a == 0 && c.body_b == 1);
    ASSERT_FLOAT_NEAR(c.penetration, 0.3f, 1e-3f);
    ASSERT_FLOAT_NEAR(c.normal[1], -1.0f, 1e-3f);

    /* Disjoint */
    capsule.position[1] = 3.0f;
    hit = khr_collide_capsule_aabb(&capsule, 0, &aabb, 1, &c);
    ASSERT_TRUE(!hit);

    /* Inverse dispatch */
    capsule.position[1] = 1.6f;
    hit = khr_collide_bodies(&aabb, 1, &capsule, 0, &c);
    ASSERT_TRUE(hit);
    ASSERT_TRUE(c.body_a == 1 && c.body_b == 0);
    ASSERT_FLOAT_NEAR(c.normal[1], 1.0f, 1e-3f);
    ASSERT_FLOAT_NEAR(c.penetration, 0.3f, 1e-3f);

    return true;
}

[[nodiscard]]
bool test_multi_shape_full_pairwise_coverage(void) {
    khr_rigid_body_t sphere = {}, aabb = {}, capsule = {};
    khr_rigid_body_init_sphere(&sphere, (float[]){ 0.0f, 0.0f, 0.0f }, 0.5f, 1.0f, 0.5f, 0.3f);
    khr_rigid_body_init_aabb(&aabb, (float[]){ 0.0f, 0.0f, 0.0f }, (float[]){ 0.5f, 0.5f, 0.5f }, 1.0f, 0.5f, 0.3f);
    khr_rigid_body_init_capsule(&capsule, (float[]){ 0.0f, -0.5f, 0.0f }, (float[]){ 0.0f, 0.5f, 0.0f }, 0.5f, 1.0f, 0.5f, 0.3f);

    khr_rigid_body_t* shapes[3] = { &sphere, &aabb, &capsule };

    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            khr_contact_t c = {};
            bool hit = khr_collide_bodies(shapes[i], (uint32_t)i, shapes[j], (uint32_t)j, &c);
            ASSERT_TRUE(hit);
            ASSERT_TRUE(c.penetration > 0.0f);
        }
    }
    return true;
}

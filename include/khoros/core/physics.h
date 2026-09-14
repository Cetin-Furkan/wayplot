#ifndef KHOROS_CORE_PHYSICS_H
#define KHOROS_CORE_PHYSICS_H

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
#include "khoros/core/spatial.h"
#include <stdint.h>
#include <stddef.h>
#include <math.h>

constexpr uint32_t KHR_PHYSICS_MAX_BODIES = 1024;
constexpr uint32_t KHR_PHYSICS_MAX_CONTACTS = 4096;
constexpr uint32_t KHR_PHYSICS_DEFAULT_SOLVER_ITERATIONS = 8;
constexpr float    KHR_PHYSICS_DEFAULT_BAUMGARTE = 0.2f;
constexpr float    KHR_PHYSICS_DEFAULT_SLOP = 0.005f;

/* Khoros Standard Physical Constants (Single Source of Truth) */
constexpr float    KHR_PHYSICS_GRAVITY_M_S2 = 9.80665f;
constexpr float    KHR_PHYSICS_FLOOR_Y      = -2.5f;
constexpr float    KHR_PHYSICS_DEFAULT_DT_S = 1.0f / 60.0f;

typedef enum {
    KHR_SHAPE_SPHERE  = 0,
    KHR_SHAPE_PLANE   = 1,
    KHR_SHAPE_AABB    = 2,
    KHR_SHAPE_CAPSULE = 3,
} khr_shape_type_t;

typedef struct {
    khr_shape_type_t type;
    union {
        struct {
            float radius;
        } sphere;
        struct {
            float normal[3]; /* Unit normal vector pointing into the free half-space */
            float distance;  /* Plane equation: dot(normal, p) - distance = 0 */
        } plane;
        struct {
            float half_extents[3];
        } aabb;
        struct {
            float p0[3];
            float p1[3];
            float radius;
        } capsule;
    };
} khr_collision_shape_t;

typedef struct {
    float position[3];
    float velocity[3];
    float force[3];
    float rotation[4];         /* Unit quaternion (x, y, z, w) */
    float angular_velocity[3]; /* Angular velocity vector in rad/s */
    float torque[3];

    float mass;
    float inv_mass;            /* 0.0f for static bodies */
    float inertia_inv[3];      /* Diagonal inverse inertia tensor in local space */

    float restitution;         /* Coefficient of restitution [0.0, 1.0] */
    float friction;            /* Coulomb friction coefficient [0.0, 1.0] */

    khr_collision_shape_t shape;

    bool  is_static;
    bool  active;
    uint32_t user_id;          /* Application / instance index */
} khr_rigid_body_t;

typedef struct {
    uint32_t body_a;           /* Index of Body A in physics world */
    uint32_t body_b;           /* Index of Body B in physics world */
    float    point[3];         /* World-space contact position */
    float    normal[3];        /* Contact normal pointing from A to B */
    float    penetration;      /* Penetration depth (> 0 when intersecting) */
    float    normal_impulse;   /* Accumulated normal impulse for clamping */
    float    tangent_impulse;  /* Accumulated tangent friction impulse */
} khr_contact_t;

typedef struct {
    khr_rigid_body_t   bodies[KHR_PHYSICS_MAX_BODIES];
    uint32_t           body_count;

    khr_contact_t      contacts[KHR_PHYSICS_MAX_CONTACTS];
    uint32_t           contact_count;

    khr_lbvh_t         broadphase_lbvh;
    khr_spatial_item_t spatial_items[KHR_PHYSICS_MAX_BODIES];

    float              gravity[3];
    uint32_t           solver_iterations;
    float              baumgarte_beta;
    float              slop;
} khr_physics_world_t;

/*
 * Vector & Quaternion Helper Functions
 */

static inline void khr_vec3_add(float r[3], const float a[3], const float b[3]) {
    r[0] = a[0] + b[0];
    r[1] = a[1] + b[1];
    r[2] = a[2] + b[2];
}

static inline void khr_vec3_sub(float r[3], const float a[3], const float b[3]) {
    r[0] = a[0] - b[0];
    r[1] = a[1] - b[1];
    r[2] = a[2] - b[2];
}

static inline void khr_vec3_scale(float r[3], const float a[3], float s) {
    r[0] = a[0] * s;
    r[1] = a[1] * s;
    r[2] = a[2] * s;
}

[[nodiscard]]
static inline float khr_vec3_dot(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static inline void khr_vec3_cross(float r[3], const float a[3], const float b[3]) {
    r[0] = a[1] * b[2] - a[2] * b[1];
    r[1] = a[2] * b[0] - a[0] * b[2];
    r[2] = a[0] * b[1] - a[1] * b[0];
}

[[nodiscard]]
static inline float khr_vec3_len_sq(const float a[3]) {
    return a[0] * a[0] + a[1] * a[1] + a[2] * a[2];
}

[[nodiscard]]
static inline float khr_vec3_len(const float a[3]) {
    return sqrtf(khr_vec3_len_sq(a));
}

static inline void khr_vec3_normalize(float r[3], const float a[3]) {
    float l = khr_vec3_len(a);
    if (l > 1e-8f) {
        float inv = 1.0f / l;
        r[0] = a[0] * inv;
        r[1] = a[1] * inv;
        r[2] = a[2] * inv;
    } else {
        r[0] = 0.0f;
        r[1] = 1.0f;
        r[2] = 0.0f;
    }
}

static inline void khr_quat_mul(float r[4], const float a[4], const float b[4]) {
    r[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    r[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    r[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    r[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

static inline void khr_quat_normalize(float q[4]) {
    float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (l > 1e-8f) {
        float inv = 1.0f / l;
        q[0] *= inv;
        q[1] *= inv;
        q[2] *= inv;
        q[3] *= inv;
    } else {
        q[0] = 0.0f;
        q[1] = 0.0f;
        q[2] = 0.0f;
        q[3] = 1.0f;
    }
}

static inline void khr_quat_rotate_vec3(float r[3], const float q[4], const float v[3]) {
    float t[3];
    khr_vec3_cross(t, q, v);
    t[0] *= 2.0f;
    t[1] *= 2.0f;
    t[2] *= 2.0f;

    float c[3];
    khr_vec3_cross(c, q, t);

    r[0] = v[0] + q[3] * t[0] + c[0];
    r[1] = v[1] + q[3] * t[1] + c[1];
    r[2] = v[2] + q[3] * t[2] + c[2];
}

/*
 * Rigid Body Initialization and Bounds Computation
 */

void khr_rigid_body_init_sphere(khr_rigid_body_t* body,
                                const float pos[3],
                                float radius,
                                float mass,
                                float restitution,
                                float friction);

void khr_rigid_body_init_plane(khr_rigid_body_t* body,
                               const float normal[3],
                               float distance,
                               float restitution,
                               float friction);

void khr_rigid_body_init_aabb(khr_rigid_body_t* body,
                              const float pos[3],
                              const float half_extents[3],
                              float mass,
                              float restitution,
                              float friction);

void khr_rigid_body_init_capsule(khr_rigid_body_t* body,
                                 const float p0[3],
                                 const float p1[3],
                                 float radius,
                                 float mass,
                                 float restitution,
                                 float friction);

void khr_rigid_body_compute_aabb(const khr_rigid_body_t* body, khr_aabb_t* out_aabb);

/*
 * Narrowphase Collision Detectors
 */

[[nodiscard]]
bool khr_collide_sphere_sphere(const khr_rigid_body_t* a, uint32_t idx_a,
                               const khr_rigid_body_t* b, uint32_t idx_b,
                               khr_contact_t* out_contact);

[[nodiscard]]
bool khr_collide_sphere_plane(const khr_rigid_body_t* sphere, uint32_t idx_sphere,
                              const khr_rigid_body_t* plane, uint32_t idx_plane,
                              khr_contact_t* out_contact);

[[nodiscard]]
bool khr_collide_sphere_aabb(const khr_rigid_body_t* sphere, uint32_t idx_sphere,
                             const khr_rigid_body_t* aabb, uint32_t idx_aabb,
                             khr_contact_t* out_contact);

[[nodiscard]]
bool khr_collide_sphere_capsule(const khr_rigid_body_t* sphere, uint32_t idx_sphere,
                                const khr_rigid_body_t* capsule, uint32_t idx_capsule,
                                khr_contact_t* out_contact);

[[nodiscard]]
bool khr_collide_aabb_aabb(const khr_rigid_body_t* a, uint32_t idx_a,
                           const khr_rigid_body_t* b, uint32_t idx_b,
                           khr_contact_t* out_contact);

[[nodiscard]]
bool khr_collide_bodies(const khr_rigid_body_t* a, uint32_t idx_a,
                        const khr_rigid_body_t* b, uint32_t idx_b,
                        khr_contact_t* out_contact);

/*
 * Physics World Simulation Management
 */

void khr_physics_world_init(khr_physics_world_t* world);

uint32_t khr_physics_world_add_body(khr_physics_world_t* world, const khr_rigid_body_t* body);

void khr_physics_world_apply_impulse(khr_physics_world_t* world,
                                     uint32_t body_index,
                                     const float impulse[3],
                                     const float rel_pos[3]);

void khr_physics_world_step(khr_physics_world_t* world, float dt);

/*
 * Energy Evaluation (Hamiltonian / Conservation of Mechanical Energy)
 */
[[nodiscard]]
float khr_rigid_body_compute_energy(const khr_rigid_body_t* body, float floor_y, float g_accel);

[[nodiscard]]
float khr_physics_world_compute_total_energy(const khr_physics_world_t* world);

#endif /* KHOROS_CORE_PHYSICS_H */

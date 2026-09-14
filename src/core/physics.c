#include "khoros/core/physics.h"
#include <string.h>

void khr_rigid_body_init_sphere(khr_rigid_body_t* body,
                                const float pos[3],
                                float radius,
                                float mass,
                                float restitution,
                                float friction) {
    if (body == nullptr) {
        return;
    }
    memset(body, 0, sizeof(*body));

    if (pos != nullptr) {
        body->position[0] = pos[0];
        body->position[1] = pos[1];
        body->position[2] = pos[2];
    }
    body->rotation[3] = 1.0f; /* Identity quaternion */

    body->mass = mass;
    if (mass > 0.0f) {
        body->inv_mass = 1.0f / mass;
        body->is_static = false;
        if (radius > 1e-4f) {
            /* I = 2/5 * m * r^2  =>  I^-1 = 2.5 / (m * r^2) */
            float i_inv = 2.5f / (mass * radius * radius);
            body->inertia_inv[0] = i_inv;
            body->inertia_inv[1] = i_inv;
            body->inertia_inv[2] = i_inv;
        }
    } else {
        body->inv_mass = 0.0f;
        body->is_static = true;
    }

    body->restitution = (restitution < 0.0f) ? 0.0f : ((restitution > 1.0f) ? 1.0f : restitution);
    body->friction = (friction < 0.0f) ? 0.0f : friction;

    body->shape.type = KHR_SHAPE_SPHERE;
    body->shape.sphere.radius = radius;
    body->active = true;
}

void khr_rigid_body_init_plane(khr_rigid_body_t* body,
                               const float normal[3],
                               float distance,
                               float restitution,
                               float friction) {
    if (body == nullptr) {
        return;
    }
    memset(body, 0, sizeof(*body));
    body->rotation[3] = 1.0f;
    body->is_static = true;
    body->active = true;

    body->restitution = (restitution < 0.0f) ? 0.0f : ((restitution > 1.0f) ? 1.0f : restitution);
    body->friction = (friction < 0.0f) ? 0.0f : friction;

    body->shape.type = KHR_SHAPE_PLANE;
    if (normal != nullptr) {
        khr_vec3_normalize(body->shape.plane.normal, normal);
    } else {
        body->shape.plane.normal[1] = 1.0f;
    }
    body->shape.plane.distance = distance;
}

void khr_rigid_body_init_aabb(khr_rigid_body_t* body,
                              const float pos[3],
                              const float half_extents[3],
                              float mass,
                              float restitution,
                              float friction) {
    if (body == nullptr) {
        return;
    }
    memset(body, 0, sizeof(*body));

    if (pos != nullptr) {
        body->position[0] = pos[0];
        body->position[1] = pos[1];
        body->position[2] = pos[2];
    }
    body->rotation[3] = 1.0f;

    body->mass = mass;
    if (mass > 0.0f) {
        body->inv_mass = 1.0f / mass;
        body->is_static = false;
        if (half_extents != nullptr) {
            float hx = half_extents[0], hy = half_extents[1], hz = half_extents[2];
            /* Box moment of inertia: I_x = 1/12 * m * ((2*hy)^2 + (2*hz)^2) = 1/3 * m * (hy^2 + hz^2) */
            float ix = (1.0f / 3.0f) * mass * (hy * hy + hz * hz);
            float iy = (1.0f / 3.0f) * mass * (hx * hx + hz * hz);
            float iz = (1.0f / 3.0f) * mass * (hx * hx + hy * hy);
            body->inertia_inv[0] = (ix > 1e-6f) ? (1.0f / ix) : 0.0f;
            body->inertia_inv[1] = (iy > 1e-6f) ? (1.0f / iy) : 0.0f;
            body->inertia_inv[2] = (iz > 1e-6f) ? (1.0f / iz) : 0.0f;
        }
    } else {
        body->inv_mass = 0.0f;
        body->is_static = true;
    }

    body->restitution = (restitution < 0.0f) ? 0.0f : ((restitution > 1.0f) ? 1.0f : restitution);
    body->friction = (friction < 0.0f) ? 0.0f : friction;

    body->shape.type = KHR_SHAPE_AABB;
    if (half_extents != nullptr) {
        body->shape.aabb.half_extents[0] = half_extents[0];
        body->shape.aabb.half_extents[1] = half_extents[1];
        body->shape.aabb.half_extents[2] = half_extents[2];
    }
    body->active = true;
}

void khr_rigid_body_init_capsule(khr_rigid_body_t* body,
                                 const float p0[3],
                                 const float p1[3],
                                 float radius,
                                 float mass,
                                 float restitution,
                                 float friction) {
    if (body == nullptr) {
        return;
    }
    memset(body, 0, sizeof(*body));

    float p0_local[3] = { 0.0f, -1.0f, 0.0f };
    float p1_local[3] = { 0.0f,  1.0f, 0.0f };
    if (p0 != nullptr) memcpy(p0_local, p0, sizeof(p0_local));
    if (p1 != nullptr) memcpy(p1_local, p1, sizeof(p1_local));

    body->position[0] = 0.5f * (p0_local[0] + p1_local[0]);
    body->position[1] = 0.5f * (p0_local[1] + p1_local[1]);
    body->position[2] = 0.5f * (p0_local[2] + p1_local[2]);
    body->rotation[3] = 1.0f;

    body->mass = mass;
    if (mass > 0.0f) {
        body->inv_mass = 1.0f / mass;
        body->is_static = false;
        float len = sqrtf(khr_vec3_len_sq(p1_local) + khr_vec3_len_sq(p0_local));
        float i_val = (1.0f / 12.0f) * mass * (3.0f * radius * radius + len * len);
        float i_inv = (i_val > 1e-6f) ? (1.0f / i_val) : 0.0f;
        body->inertia_inv[0] = i_inv;
        body->inertia_inv[1] = (radius > 1e-4f) ? (2.0f / (mass * radius * radius)) : 0.0f;
        body->inertia_inv[2] = i_inv;
    } else {
        body->inv_mass = 0.0f;
        body->is_static = true;
    }

    body->restitution = (restitution < 0.0f) ? 0.0f : ((restitution > 1.0f) ? 1.0f : restitution);
    body->friction = (friction < 0.0f) ? 0.0f : friction;

    body->shape.type = KHR_SHAPE_CAPSULE;
    body->shape.capsule.p0[0] = p0_local[0] - body->position[0];
    body->shape.capsule.p0[1] = p0_local[1] - body->position[1];
    body->shape.capsule.p0[2] = p0_local[2] - body->position[2];
    body->shape.capsule.p1[0] = p1_local[0] - body->position[0];
    body->shape.capsule.p1[1] = p1_local[1] - body->position[1];
    body->shape.capsule.p1[2] = p1_local[2] - body->position[2];
    body->shape.capsule.radius = radius;
    body->active = true;
}

void khr_rigid_body_compute_aabb(const khr_rigid_body_t* body, khr_aabb_t* out_aabb) {
    if (body == nullptr || out_aabb == nullptr) {
        return;
    }

    switch (body->shape.type) {
    case KHR_SHAPE_SPHERE: {
        float r = body->shape.sphere.radius;
        out_aabb->min[0] = body->position[0] - r;
        out_aabb->min[1] = body->position[1] - r;
        out_aabb->min[2] = body->position[2] - r;
        out_aabb->max[0] = body->position[0] + r;
        out_aabb->max[1] = body->position[1] + r;
        out_aabb->max[2] = body->position[2] + r;
        break;
    }
    case KHR_SHAPE_PLANE: {
        out_aabb->min[0] = -1000.0f;
        out_aabb->min[1] = -1000.0f;
        out_aabb->min[2] = -1000.0f;
        out_aabb->max[0] = 1000.0f;
        out_aabb->max[1] = 1000.0f;
        out_aabb->max[2] = 1000.0f;
        break;
    }
    case KHR_SHAPE_AABB: {
        float hx = body->shape.aabb.half_extents[0];
        float hy = body->shape.aabb.half_extents[1];
        float hz = body->shape.aabb.half_extents[2];
        out_aabb->min[0] = body->position[0] - hx;
        out_aabb->min[1] = body->position[1] - hy;
        out_aabb->min[2] = body->position[2] - hz;
        out_aabb->max[0] = body->position[0] + hx;
        out_aabb->max[1] = body->position[1] + hy;
        out_aabb->max[2] = body->position[2] + hz;
        break;
    }
    case KHR_SHAPE_CAPSULE: {
        float p0_w[3], p1_w[3];
        khr_quat_rotate_vec3(p0_w, body->rotation, body->shape.capsule.p0);
        khr_quat_rotate_vec3(p1_w, body->rotation, body->shape.capsule.p1);
        khr_vec3_add(p0_w, p0_w, body->position);
        khr_vec3_add(p1_w, p1_w, body->position);

        float r = body->shape.capsule.radius;
        out_aabb->min[0] = fminf(p0_w[0], p1_w[0]) - r;
        out_aabb->min[1] = fminf(p0_w[1], p1_w[1]) - r;
        out_aabb->min[2] = fminf(p0_w[2], p1_w[2]) - r;
        out_aabb->max[0] = fmaxf(p0_w[0], p1_w[0]) + r;
        out_aabb->max[1] = fmaxf(p0_w[1], p1_w[1]) + r;
        out_aabb->max[2] = fmaxf(p0_w[2], p1_w[2]) + r;
        break;
    }
    default:
        *out_aabb = khr_aabb_make_empty();
        break;
    }
}

[[nodiscard]]
bool khr_collide_sphere_sphere(const khr_rigid_body_t* a, uint32_t idx_a,
                               const khr_rigid_body_t* b, uint32_t idx_b,
                               khr_contact_t* out_contact) {
    if (a == nullptr || b == nullptr || out_contact == nullptr) {
        return false;
    }

    float delta[3];
    khr_vec3_sub(delta, b->position, a->position);
    float d2 = khr_vec3_len_sq(delta);
    float r_sum = a->shape.sphere.radius + b->shape.sphere.radius;

    if (d2 >= r_sum * r_sum) {
        return false;
    }

    float d = sqrtf(d2);
    out_contact->body_a = idx_a;
    out_contact->body_b = idx_b;
    out_contact->normal_impulse = 0.0f;
    out_contact->tangent_impulse = 0.0f;

    if (d < 1e-6f) {
        out_contact->normal[0] = 0.0f;
        out_contact->normal[1] = 1.0f;
        out_contact->normal[2] = 0.0f;
        out_contact->penetration = r_sum;
        out_contact->point[0] = a->position[0];
        out_contact->point[1] = a->position[1];
        out_contact->point[2] = a->position[2];
    } else {
        float inv_d = 1.0f / d;
        out_contact->normal[0] = delta[0] * inv_d;
        out_contact->normal[1] = delta[1] * inv_d;
        out_contact->normal[2] = delta[2] * inv_d;
        out_contact->penetration = r_sum - d;
        float arm = a->shape.sphere.radius - 0.5f * out_contact->penetration;
        out_contact->point[0] = a->position[0] + out_contact->normal[0] * arm;
        out_contact->point[1] = a->position[1] + out_contact->normal[1] * arm;
        out_contact->point[2] = a->position[2] + out_contact->normal[2] * arm;
    }
    return true;
}

[[nodiscard]]
bool khr_collide_sphere_plane(const khr_rigid_body_t* sphere, uint32_t idx_sphere,
                              const khr_rigid_body_t* plane, uint32_t idx_plane,
                              khr_contact_t* out_contact) {
    if (sphere == nullptr || plane == nullptr || out_contact == nullptr) {
        return false;
    }

    const float* n = plane->shape.plane.normal;
    float dist = khr_vec3_dot(n, sphere->position) - plane->shape.plane.distance;
    float r = sphere->shape.sphere.radius;

    if (dist >= r) {
        return false;
    }

    out_contact->body_a = idx_plane;
    out_contact->body_b = idx_sphere;
    out_contact->normal[0] = n[0];
    out_contact->normal[1] = n[1];
    out_contact->normal[2] = n[2];
    out_contact->penetration = r - dist;
    out_contact->normal_impulse = 0.0f;
    out_contact->tangent_impulse = 0.0f;

    out_contact->point[0] = sphere->position[0] - n[0] * dist;
    out_contact->point[1] = sphere->position[1] - n[1] * dist;
    out_contact->point[2] = sphere->position[2] - n[2] * dist;
    return true;
}

[[nodiscard]]
bool khr_collide_sphere_aabb(const khr_rigid_body_t* sphere, uint32_t idx_sphere,
                             const khr_rigid_body_t* aabb, uint32_t idx_aabb,
                             khr_contact_t* out_contact) {
    if (sphere == nullptr || aabb == nullptr || out_contact == nullptr) {
        return false;
    }

    float box_min[3], box_max[3];
    for (int i = 0; i < 3; i++) {
        box_min[i] = aabb->position[i] - aabb->shape.aabb.half_extents[i];
        box_max[i] = aabb->position[i] + aabb->shape.aabb.half_extents[i];
    }

    float closest[3];
    for (int i = 0; i < 3; i++) {
        float p = sphere->position[i];
        if (p < box_min[i]) p = box_min[i];
        if (p > box_max[i]) p = box_max[i];
        closest[i] = p;
    }

    float delta[3];
    khr_vec3_sub(delta, sphere->position, closest);
    float d2 = khr_vec3_len_sq(delta);
    float r = sphere->shape.sphere.radius;

    if (d2 >= r * r && d2 > 1e-12f) {
        return false;
    }

    out_contact->body_a = idx_aabb;
    out_contact->body_b = idx_sphere;
    out_contact->normal_impulse = 0.0f;
    out_contact->tangent_impulse = 0.0f;

    if (d2 < 1e-12f) {
        /* Sphere center inside AABB: find closest face */
        float min_dist = 1e30f;
        int best_axis = 1;
        float best_sign = 1.0f;

        for (int i = 0; i < 3; i++) {
            float d_min = sphere->position[i] - box_min[i];
            float d_max = box_max[i] - sphere->position[i];
            if (d_min < min_dist) {
                min_dist = d_min;
                best_axis = i;
                best_sign = -1.0f;
            }
            if (d_max < min_dist) {
                min_dist = d_max;
                best_axis = i;
                best_sign = 1.0f;
            }
        }

        out_contact->normal[0] = (best_axis == 0) ? best_sign : 0.0f;
        out_contact->normal[1] = (best_axis == 1) ? best_sign : 0.0f;
        out_contact->normal[2] = (best_axis == 2) ? best_sign : 0.0f;
        out_contact->penetration = r + min_dist;
        out_contact->point[0] = closest[0];
        out_contact->point[1] = closest[1];
        out_contact->point[2] = closest[2];
    } else {
        float d = sqrtf(d2);
        out_contact->normal[0] = delta[0] / d;
        out_contact->normal[1] = delta[1] / d;
        out_contact->normal[2] = delta[2] / d;
        out_contact->penetration = r - d;
        out_contact->point[0] = closest[0];
        out_contact->point[1] = closest[1];
        out_contact->point[2] = closest[2];
    }
    return true;
}

[[nodiscard]]
bool khr_collide_sphere_capsule(const khr_rigid_body_t* sphere, uint32_t idx_sphere,
                                const khr_rigid_body_t* capsule, uint32_t idx_capsule,
                                khr_contact_t* out_contact) {
    if (sphere == nullptr || capsule == nullptr || out_contact == nullptr) {
        return false;
    }

    float p0_w[3], p1_w[3];
    khr_quat_rotate_vec3(p0_w, capsule->rotation, capsule->shape.capsule.p0);
    khr_quat_rotate_vec3(p1_w, capsule->rotation, capsule->shape.capsule.p1);
    khr_vec3_add(p0_w, p0_w, capsule->position);
    khr_vec3_add(p1_w, p1_w, capsule->position);

    float seg[3];
    khr_vec3_sub(seg, p1_w, p0_w);
    float seg_len2 = khr_vec3_len_sq(seg);

    float t = 0.0f;
    if (seg_len2 > 1e-8f) {
        float diff[3];
        khr_vec3_sub(diff, sphere->position, p0_w);
        t = khr_vec3_dot(diff, seg) / seg_len2;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
    }

    float closest[3];
    closest[0] = p0_w[0] + seg[0] * t;
    closest[1] = p0_w[1] + seg[1] * t;
    closest[2] = p0_w[2] + seg[2] * t;

    float delta[3];
    khr_vec3_sub(delta, sphere->position, closest);
    float d2 = khr_vec3_len_sq(delta);
    float r_sum = sphere->shape.sphere.radius + capsule->shape.capsule.radius;

    if (d2 >= r_sum * r_sum) {
        return false;
    }

    float d = sqrtf(d2);
    out_contact->body_a = idx_capsule;
    out_contact->body_b = idx_sphere;
    out_contact->normal_impulse = 0.0f;
    out_contact->tangent_impulse = 0.0f;

    if (d < 1e-6f) {
        out_contact->normal[0] = 0.0f;
        out_contact->normal[1] = 1.0f;
        out_contact->normal[2] = 0.0f;
        out_contact->penetration = r_sum;
        out_contact->point[0] = closest[0];
        out_contact->point[1] = closest[1];
        out_contact->point[2] = closest[2];
    } else {
        out_contact->normal[0] = delta[0] / d;
        out_contact->normal[1] = delta[1] / d;
        out_contact->normal[2] = delta[2] / d;
        out_contact->penetration = r_sum - d;
        float arm = capsule->shape.capsule.radius - 0.5f * out_contact->penetration;
        out_contact->point[0] = closest[0] + out_contact->normal[0] * arm;
        out_contact->point[1] = closest[1] + out_contact->normal[1] * arm;
        out_contact->point[2] = closest[2] + out_contact->normal[2] * arm;
    }
    return true;
}

[[nodiscard]]
bool khr_collide_aabb_aabb(const khr_rigid_body_t* a, uint32_t idx_a,
                           const khr_rigid_body_t* b, uint32_t idx_b,
                           khr_contact_t* out_contact) {
    if (a == nullptr || b == nullptr || out_contact == nullptr) {
        return false;
    }

    float delta[3];
    khr_vec3_sub(delta, b->position, a->position);

    float overlap[3];
    for (int i = 0; i < 3; i++) {
        float h_sum = a->shape.aabb.half_extents[i] + b->shape.aabb.half_extents[i];
        overlap[i] = h_sum - fabsf(delta[i]);
        if (overlap[i] <= 0.0f) {
            return false;
        }
    }

    int min_axis = 0;
    float min_overlap = overlap[0];
    if (overlap[1] < min_overlap) {
        min_overlap = overlap[1];
        min_axis = 1;
    }
    if (overlap[2] < min_overlap) {
        min_overlap = overlap[2];
        min_axis = 2;
    }

    out_contact->body_a = idx_a;
    out_contact->body_b = idx_b;
    out_contact->normal[0] = 0.0f;
    out_contact->normal[1] = 0.0f;
    out_contact->normal[2] = 0.0f;
    out_contact->normal[min_axis] = (delta[min_axis] >= 0.0f) ? 1.0f : -1.0f;
    out_contact->penetration = min_overlap;
    out_contact->normal_impulse = 0.0f;
    out_contact->tangent_impulse = 0.0f;

    for (int i = 0; i < 3; i++) {
        out_contact->point[i] = 0.5f * (a->position[i] + b->position[i]);
    }
    return true;
}

[[nodiscard]]
bool khr_collide_aabb_plane(const khr_rigid_body_t* aabb, uint32_t idx_aabb,
                            const khr_rigid_body_t* plane, uint32_t idx_plane,
                            khr_contact_t* out_contact) {
    if (aabb == nullptr || plane == nullptr || out_contact == nullptr) {
        return false;
    }
    const float* n = plane->shape.plane.normal;
    float hx = aabb->shape.aabb.half_extents[0];
    float hy = aabb->shape.aabb.half_extents[1];
    float hz = aabb->shape.aabb.half_extents[2];
    float r = hx * fabsf(n[0]) + hy * fabsf(n[1]) + hz * fabsf(n[2]);
    float dist = khr_vec3_dot(n, aabb->position) - plane->shape.plane.distance;
    if (dist >= r) {
        return false;
    }
    out_contact->body_a = idx_plane;
    out_contact->body_b = idx_aabb;
    out_contact->normal[0] = n[0];
    out_contact->normal[1] = n[1];
    out_contact->normal[2] = n[2];
    out_contact->penetration = r - dist;
    out_contact->normal_impulse = 0.0f;
    out_contact->tangent_impulse = 0.0f;
    out_contact->point[0] = aabb->position[0] - n[0] * dist;
    out_contact->point[1] = aabb->position[1] - n[1] * dist;
    out_contact->point[2] = aabb->position[2] - n[2] * dist;
    return true;
}

[[nodiscard]]
bool khr_collide_capsule_plane(const khr_rigid_body_t* capsule, uint32_t idx_capsule,
                               const khr_rigid_body_t* plane, uint32_t idx_plane,
                               khr_contact_t* out_contact) {
    if (capsule == nullptr || plane == nullptr || out_contact == nullptr) {
        return false;
    }
    const float* n = plane->shape.plane.normal;
    float p0_w[3], p1_w[3];
    khr_quat_rotate_vec3(p0_w, capsule->rotation, capsule->shape.capsule.p0);
    khr_quat_rotate_vec3(p1_w, capsule->rotation, capsule->shape.capsule.p1);
    khr_vec3_add(p0_w, p0_w, capsule->position);
    khr_vec3_add(p1_w, p1_w, capsule->position);

    float d0 = khr_vec3_dot(n, p0_w) - plane->shape.plane.distance;
    float d1 = khr_vec3_dot(n, p1_w) - plane->shape.plane.distance;

    float r = capsule->shape.capsule.radius;
    float min_d = (d0 < d1) ? d0 : d1;
    const float* min_p = (d0 < d1) ? p0_w : p1_w;

    if (min_d >= r) {
        return false;
    }

    out_contact->body_a = idx_plane;
    out_contact->body_b = idx_capsule;
    out_contact->normal[0] = n[0];
    out_contact->normal[1] = n[1];
    out_contact->normal[2] = n[2];
    out_contact->penetration = r - min_d;
    out_contact->normal_impulse = 0.0f;
    out_contact->tangent_impulse = 0.0f;
    out_contact->point[0] = min_p[0] - n[0] * min_d;
    out_contact->point[1] = min_p[1] - n[1] * min_d;
    out_contact->point[2] = min_p[2] - n[2] * min_d;
    return true;
}

[[nodiscard]]
bool khr_collide_bodies(const khr_rigid_body_t* a, uint32_t idx_a,
                        const khr_rigid_body_t* b, uint32_t idx_b,
                        khr_contact_t* out_contact) {
    if (a == nullptr || b == nullptr || out_contact == nullptr) {
        return false;
    }

    if (a->shape.type == KHR_SHAPE_SPHERE && b->shape.type == KHR_SHAPE_SPHERE) {
        return khr_collide_sphere_sphere(a, idx_a, b, idx_b, out_contact);
    }
    if (a->shape.type == KHR_SHAPE_SPHERE && b->shape.type == KHR_SHAPE_PLANE) {
        return khr_collide_sphere_plane(a, idx_a, b, idx_b, out_contact);
    }
    if (a->shape.type == KHR_SHAPE_PLANE && b->shape.type == KHR_SHAPE_SPHERE) {
        return khr_collide_sphere_plane(b, idx_b, a, idx_a, out_contact);
    }
    if (a->shape.type == KHR_SHAPE_SPHERE && b->shape.type == KHR_SHAPE_AABB) {
        return khr_collide_sphere_aabb(a, idx_a, b, idx_b, out_contact);
    }
    if (a->shape.type == KHR_SHAPE_AABB && b->shape.type == KHR_SHAPE_SPHERE) {
        return khr_collide_sphere_aabb(b, idx_b, a, idx_a, out_contact);
    }
    if (a->shape.type == KHR_SHAPE_SPHERE && b->shape.type == KHR_SHAPE_CAPSULE) {
        return khr_collide_sphere_capsule(a, idx_a, b, idx_b, out_contact);
    }
    if (a->shape.type == KHR_SHAPE_CAPSULE && b->shape.type == KHR_SHAPE_SPHERE) {
        return khr_collide_sphere_capsule(b, idx_b, a, idx_a, out_contact);
    }
    if (a->shape.type == KHR_SHAPE_AABB && b->shape.type == KHR_SHAPE_AABB) {
        return khr_collide_aabb_aabb(a, idx_a, b, idx_b, out_contact);
    }
    if (a->shape.type == KHR_SHAPE_AABB && b->shape.type == KHR_SHAPE_PLANE) {
        return khr_collide_aabb_plane(a, idx_a, b, idx_b, out_contact);
    }
    if (a->shape.type == KHR_SHAPE_PLANE && b->shape.type == KHR_SHAPE_AABB) {
        return khr_collide_aabb_plane(b, idx_b, a, idx_a, out_contact);
    }
    if (a->shape.type == KHR_SHAPE_CAPSULE && b->shape.type == KHR_SHAPE_PLANE) {
        return khr_collide_capsule_plane(a, idx_a, b, idx_b, out_contact);
    }
    if (a->shape.type == KHR_SHAPE_PLANE && b->shape.type == KHR_SHAPE_CAPSULE) {
        return khr_collide_capsule_plane(b, idx_b, a, idx_a, out_contact);
    }
    return false;
}

void khr_physics_world_init(khr_physics_world_t* world) {
    if (world == nullptr) {
        return;
    }
    memset(world, 0, sizeof(*world));
    world->gravity[0] = 0.0f;
    world->gravity[1] = -KHR_PHYSICS_GRAVITY_M_S2;
    world->gravity[2] = 0.0f;
    world->solver_iterations = KHR_PHYSICS_DEFAULT_SOLVER_ITERATIONS;
    world->baumgarte_beta = KHR_PHYSICS_DEFAULT_BAUMGARTE;
    world->slop = KHR_PHYSICS_DEFAULT_SLOP;
}

uint32_t khr_physics_world_add_body(khr_physics_world_t* world, const khr_rigid_body_t* body) {
    if (world == nullptr || body == nullptr || world->body_count >= KHR_PHYSICS_MAX_BODIES) {
        return UINT32_MAX;
    }
    uint32_t idx = world->body_count++;
    world->bodies[idx] = *body;
    return idx;
}

void khr_physics_world_apply_impulse(khr_physics_world_t* world,
                                     uint32_t body_index,
                                     const float impulse[3],
                                     const float rel_pos[3]) {
    if (world == nullptr || body_index >= world->body_count || impulse == nullptr) {
        return;
    }
    khr_rigid_body_t* b = &world->bodies[body_index];
    if (!b->active || b->is_static) {
        return;
    }

    b->velocity[0] += b->inv_mass * impulse[0];
    b->velocity[1] += b->inv_mass * impulse[1];
    b->velocity[2] += b->inv_mass * impulse[2];

    if (rel_pos != nullptr) {
        float r_cross_j[3];
        khr_vec3_cross(r_cross_j, rel_pos, impulse);
        b->angular_velocity[0] += b->inertia_inv[0] * r_cross_j[0];
        b->angular_velocity[1] += b->inertia_inv[1] * r_cross_j[1];
        b->angular_velocity[2] += b->inertia_inv[2] * r_cross_j[2];
    }
}

void khr_physics_world_step(khr_physics_world_t* world, float dt) {
    if (world == nullptr || dt <= 0.0f) {
        return;
    }
    if (dt > 0.05f) {
        dt = 0.05f; /* Clamp maximum substep duration to guarantee stability */
    }

    /* 1. Broadphase: Compute AABBs and build Morton LBVH for spatial queries */
    uint32_t active_count = 0;
    for (uint32_t i = 0; i < world->body_count; i++) {
        if (!world->bodies[i].active || world->bodies[i].shape.type == KHR_SHAPE_PLANE) {
            continue;
        }
        khr_aabb_t box;
        khr_rigid_body_compute_aabb(&world->bodies[i], &box);
        world->spatial_items[active_count] = (khr_spatial_item_t){
            .id = i,
            .morton_code = 0,
            .bounds = box,
        };
        active_count++;
    }

    if (active_count > 0) {
        (void)khr_lbvh_build(&world->broadphase_lbvh, world->spatial_items, active_count);
    }

    /* 2. Narrowphase: Detect contact manifolds across candidate pairs */
    world->contact_count = 0;

    /* A. Static Planes vs Dynamic Bodies */
    for (uint32_t p = 0; p < world->body_count; p++) {
        if (!world->bodies[p].active || world->bodies[p].shape.type != KHR_SHAPE_PLANE) {
            continue;
        }
        for (uint32_t b = 0; b < world->body_count; b++) {
            if (p == b || !world->bodies[b].active || world->bodies[b].is_static) {
                continue;
            }
            if (world->contact_count >= KHR_PHYSICS_MAX_CONTACTS) {
                break;
            }
            khr_contact_t c = {};
            if (khr_collide_bodies(&world->bodies[p], p, &world->bodies[b], b, &c)) {
                world->contacts[world->contact_count++] = c;
            }
        }
    }

    /* B. LBVH Broadphase Query for Body Pairs */
    for (uint32_t k = 0; k < active_count; k++) {
        uint32_t i = world->spatial_items[k].id;
        if (world->bodies[i].is_static) {
            continue;
        }

        uint32_t cand_ids[KHR_LBVH_MAX_LEAVES];
        uint32_t cand_count = khr_lbvh_query_aabb(&world->broadphase_lbvh,
                                                  &world->spatial_items[k].bounds,
                                                  cand_ids, KHR_LBVH_MAX_LEAVES);
        for (uint32_t c = 0; c < cand_count; c++) {
            uint32_t j = cand_ids[c];
            if (j <= i) {
                continue; /* Avoid self-collision and duplicate pairs */
            }
            if (world->bodies[i].is_static && world->bodies[j].is_static) {
                continue;
            }
            if (world->contact_count >= KHR_PHYSICS_MAX_CONTACTS) {
                break;
            }
            khr_contact_t contact = {};
            if (khr_collide_bodies(&world->bodies[i], i, &world->bodies[j], j, &contact)) {
                world->contacts[world->contact_count++] = contact;
            }
        }
    }

    /* 3. Integrate External Forces and Gravity */
    for (uint32_t i = 0; i < world->body_count; i++) {
        khr_rigid_body_t* b = &world->bodies[i];
        if (!b->active || b->is_static) {
            continue;
        }

        b->velocity[0] += (world->gravity[0] + b->force[0] * b->inv_mass) * dt;
        b->velocity[1] += (world->gravity[1] + b->force[1] * b->inv_mass) * dt;
        b->velocity[2] += (world->gravity[2] + b->force[2] * b->inv_mass) * dt;

        b->angular_velocity[0] += b->inertia_inv[0] * b->torque[0] * dt;
        b->angular_velocity[1] += b->inertia_inv[1] * b->torque[1] * dt;
        b->angular_velocity[2] += b->inertia_inv[2] * b->torque[2] * dt;
    }

    /* 4. Sequential Impulse Resolution (Normal + Coulomb Friction + Baumgarte) */
    uint32_t iters = world->solver_iterations > 0 ? world->solver_iterations : KHR_PHYSICS_DEFAULT_SOLVER_ITERATIONS;
    float beta = world->baumgarte_beta > 0.0f ? world->baumgarte_beta : KHR_PHYSICS_DEFAULT_BAUMGARTE;
    float slop = world->slop > 0.0f ? world->slop : KHR_PHYSICS_DEFAULT_SLOP;

    /* Pre-solve contact restitution bias once prior to velocity iterations */
    float bounce_bias[KHR_PHYSICS_MAX_CONTACTS];
    for (uint32_t c = 0; c < world->contact_count; c++) {
        khr_contact_t* contact = &world->contacts[c];
        khr_rigid_body_t* bA = &world->bodies[contact->body_a];
        khr_rigid_body_t* bB = &world->bodies[contact->body_b];

        float rA[3], rB[3];
        khr_vec3_sub(rA, contact->point, bA->position);
        khr_vec3_sub(rB, contact->point, bB->position);

        float angA[3], angB[3];
        khr_vec3_cross(angA, bA->angular_velocity, rA);
        khr_vec3_cross(angB, bB->angular_velocity, rB);

        float vA[3], vB[3];
        khr_vec3_add(vA, bA->velocity, angA);
        khr_vec3_add(vB, bB->velocity, angB);

        float v_rel[3];
        khr_vec3_sub(v_rel, vB, vA);
        float vn = khr_vec3_dot(v_rel, contact->normal);

        float rest = fminf(bA->restitution, bB->restitution);
        bounce_bias[c] = (vn < -0.5f) ? (-rest * vn) : 0.0f;
    }

    for (uint32_t iter = 0; iter < iters; iter++) {
        for (uint32_t c = 0; c < world->contact_count; c++) {
            khr_contact_t* contact = &world->contacts[c];
            khr_rigid_body_t* bA = &world->bodies[contact->body_a];
            khr_rigid_body_t* bB = &world->bodies[contact->body_b];

            float rA[3], rB[3];
            khr_vec3_sub(rA, contact->point, bA->position);
            khr_vec3_sub(rB, contact->point, bB->position);

            /* Relative velocity at contact point: v_rel = (vB + wB x rB) - (vA + wA x rA) */
            float angA[3], angB[3];
            khr_vec3_cross(angA, bA->angular_velocity, rA);
            khr_vec3_cross(angB, bB->angular_velocity, rB);

            float vA[3], vB[3];
            khr_vec3_add(vA, bA->velocity, angA);
            khr_vec3_add(vB, bB->velocity, angB);

            float v_rel[3];
            khr_vec3_sub(v_rel, vB, vA);

            float vn = khr_vec3_dot(v_rel, contact->normal);

            /* Effective mass in normal direction: K_n */
            float uA[3], uB[3];
            khr_vec3_cross(uA, rA, contact->normal);
            khr_vec3_cross(uB, rB, contact->normal);

            float wA[3] = { bA->inertia_inv[0] * uA[0], bA->inertia_inv[1] * uA[1], bA->inertia_inv[2] * uA[2] };
            float wB[3] = { bB->inertia_inv[0] * uB[0], bB->inertia_inv[1] * uB[1], bB->inertia_inv[2] * uB[2] };

            float tA[3], tB[3];
            khr_vec3_cross(tA, wA, rA);
            khr_vec3_cross(tB, wB, rB);

            float Kn = bA->inv_mass + bB->inv_mass + khr_vec3_dot(tA, contact->normal) + khr_vec3_dot(tB, contact->normal);
            if (Kn < 1e-7f) {
                continue;
            }

            /* Restitution bias + Baumgarte stabilization */
            float pen_bias = (beta / dt) * fmaxf(0.0f, contact->penetration - slop);
            float bias = pen_bias + bounce_bias[c];

            float d_lambda_n = -(vn - bias) / Kn;
            float old_lambda_n = contact->normal_impulse;
            contact->normal_impulse = fmaxf(0.0f, old_lambda_n + d_lambda_n);
            float d_pn = contact->normal_impulse - old_lambda_n;

            float jn[3];
            khr_vec3_scale(jn, contact->normal, d_pn);

            if (!bA->is_static) {
                bA->velocity[0] -= bA->inv_mass * jn[0];
                bA->velocity[1] -= bA->inv_mass * jn[1];
                bA->velocity[2] -= bA->inv_mass * jn[2];

                float rA_cross_jn[3];
                khr_vec3_cross(rA_cross_jn, rA, jn);
                bA->angular_velocity[0] -= bA->inertia_inv[0] * rA_cross_jn[0];
                bA->angular_velocity[1] -= bA->inertia_inv[1] * rA_cross_jn[1];
                bA->angular_velocity[2] -= bA->inertia_inv[2] * rA_cross_jn[2];
            }
            if (!bB->is_static) {
                bB->velocity[0] += bB->inv_mass * jn[0];
                bB->velocity[1] += bB->inv_mass * jn[1];
                bB->velocity[2] += bB->inv_mass * jn[2];

                float rB_cross_jn[3];
                khr_vec3_cross(rB_cross_jn, rB, jn);
                bB->angular_velocity[0] += bB->inertia_inv[0] * rB_cross_jn[0];
                bB->angular_velocity[1] += bB->inertia_inv[1] * rB_cross_jn[1];
                bB->angular_velocity[2] += bB->inertia_inv[2] * rB_cross_jn[2];
            }

            /* Tangential Coulomb Friction Impulse */
            khr_vec3_cross(angA, bA->angular_velocity, rA);
            khr_vec3_cross(angB, bB->angular_velocity, rB);
            khr_vec3_add(vA, bA->velocity, angA);
            khr_vec3_add(vB, bB->velocity, angB);
            khr_vec3_sub(v_rel, vB, vA);

            float vn_curr = khr_vec3_dot(v_rel, contact->normal);
            float v_tangent[3] = {
                v_rel[0] - vn_curr * contact->normal[0],
                v_rel[1] - vn_curr * contact->normal[1],
                v_rel[2] - vn_curr * contact->normal[2],
            };

            float tan_speed = khr_vec3_len(v_tangent);
            if (tan_speed > 1e-5f) {
                float t_dir[3];
                khr_vec3_scale(t_dir, v_tangent, 1.0f / tan_speed);

                float utA[3], utB[3];
                khr_vec3_cross(utA, rA, t_dir);
                khr_vec3_cross(utB, rB, t_dir);

                float wtA[3] = { bA->inertia_inv[0] * utA[0], bA->inertia_inv[1] * utA[1], bA->inertia_inv[2] * utA[2] };
                float wtB[3] = { bB->inertia_inv[0] * utB[0], bB->inertia_inv[1] * utB[1], bB->inertia_inv[2] * utB[2] };

                float ttA[3], ttB[3];
                khr_vec3_cross(ttA, wtA, rA);
                khr_vec3_cross(ttB, wtB, rB);

                float Kt = bA->inv_mass + bB->inv_mass + khr_vec3_dot(ttA, t_dir) + khr_vec3_dot(ttB, t_dir);
                if (Kt > 1e-7f) {
                    float d_lambda_t = -khr_vec3_dot(v_rel, t_dir) / Kt;
                    float mu = sqrtf(bA->friction * bB->friction);
                    float max_f = mu * contact->normal_impulse;

                    float old_lambda_t = contact->tangent_impulse;
                    contact->tangent_impulse = fminf(max_f, fmaxf(-max_f, old_lambda_t + d_lambda_t));
                    float d_pt = contact->tangent_impulse - old_lambda_t;

                    float jt[3];
                    khr_vec3_scale(jt, t_dir, d_pt);

                    if (!bA->is_static) {
                        bA->velocity[0] -= bA->inv_mass * jt[0];
                        bA->velocity[1] -= bA->inv_mass * jt[1];
                        bA->velocity[2] -= bA->inv_mass * jt[2];

                        float rA_cross_jt[3];
                        khr_vec3_cross(rA_cross_jt, rA, jt);
                        bA->angular_velocity[0] -= bA->inertia_inv[0] * rA_cross_jt[0];
                        bA->angular_velocity[1] -= bA->inertia_inv[1] * rA_cross_jt[1];
                        bA->angular_velocity[2] -= bA->inertia_inv[2] * rA_cross_jt[2];
                    }
                    if (!bB->is_static) {
                        bB->velocity[0] += bB->inv_mass * jt[0];
                        bB->velocity[1] += bB->inv_mass * jt[1];
                        bB->velocity[2] += bB->inv_mass * jt[2];

                        float rB_cross_jt[3];
                        khr_vec3_cross(rB_cross_jt, rB, jt);
                        bB->angular_velocity[0] += bB->inertia_inv[0] * rB_cross_jt[0];
                        bB->angular_velocity[1] += bB->inertia_inv[1] * rB_cross_jt[1];
                        bB->angular_velocity[2] += bB->inertia_inv[2] * rB_cross_jt[2];
                    }
                }
            }
        }
    }

    /* 5. Symplectic Semi-Implicit Euler Integration (Positions & Orientations) */
    for (uint32_t i = 0; i < world->body_count; i++) {
        khr_rigid_body_t* b = &world->bodies[i];
        if (!b->active || b->is_static) {
            continue;
        }

        /* Position integration: x += v * dt */
        b->position[0] += b->velocity[0] * dt;
        b->position[1] += b->velocity[1] * dt;
        b->position[2] += b->velocity[2] * dt;

        /* Orientation integration: dq = 0.5 * [omega, 0] * q * dt */
        float w_quat[4] = { b->angular_velocity[0], b->angular_velocity[1], b->angular_velocity[2], 0.0f };
        float dq[4];
        khr_quat_mul(dq, w_quat, b->rotation);

        b->rotation[0] += 0.5f * dq[0] * dt;
        b->rotation[1] += 0.5f * dq[1] * dt;
        b->rotation[2] += 0.5f * dq[2] * dt;
        b->rotation[3] += 0.5f * dq[3] * dt;
        khr_quat_normalize(b->rotation);

        /* Angular damping */
        b->angular_velocity[0] *= 0.995f;
        b->angular_velocity[1] *= 0.995f;
        b->angular_velocity[2] *= 0.995f;

        /* Clear transient forces and torques */
        b->force[0] = 0.0f;
        b->force[1] = 0.0f;
        b->force[2] = 0.0f;
        b->torque[0] = 0.0f;
        b->torque[1] = 0.0f;
        b->torque[2] = 0.0f;
    }
}

float khr_rigid_body_compute_energy(const khr_rigid_body_t* body, float floor_y, float g_accel) {
    if (body == nullptr || !body->active || body->is_static) {
        return 0.0f;
    }
    /* Translational kinetic energy: 0.5 * m * v^2 */
    float v_sq = body->velocity[0] * body->velocity[0] +
                 body->velocity[1] * body->velocity[1] +
                 body->velocity[2] * body->velocity[2];
    float ke_trans = 0.5f * body->mass * v_sq;

    /* Rotational kinetic energy: 0.5 * (I_x*w_x^2 + I_y*w_y^2 + I_z*w_z^2) */
    float ke_rot = 0.0f;
    for (int i = 0; i < 3; i++) {
        if (body->inertia_inv[i] > 1e-7f) {
            float I_i = 1.0f / body->inertia_inv[i];
            ke_rot += 0.5f * I_i * body->angular_velocity[i] * body->angular_velocity[i];
        }
    }

    /* Gravitational potential energy: m * g * (y - floor_y) */
    float h = body->position[1] - floor_y;
    if (h < 0.0f) {
        h = 0.0f;
    }
    float pe = body->mass * g_accel * h;

    return ke_trans + ke_rot + pe;
}

float khr_physics_world_compute_total_energy(const khr_physics_world_t* world) {
    if (world == nullptr) {
        return 0.0f;
    }
    float g = sqrtf(world->gravity[0] * world->gravity[0] +
                    world->gravity[1] * world->gravity[1] +
                    world->gravity[2] * world->gravity[2]);
    float total_e = 0.0f;
    for (uint32_t i = 0; i < world->body_count; i++) {
        total_e += khr_rigid_body_compute_energy(&world->bodies[i], KHR_PHYSICS_FLOOR_Y, g);
    }
    return total_e;
}

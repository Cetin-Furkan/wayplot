#ifndef KHOROS_CORE_SPATIAL_H
#define KHOROS_CORE_SPATIAL_H

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
#include <stdint.h>
#include <stddef.h>

constexpr uint32_t KHR_LBVH_MAX_LEAVES = 1024;
constexpr uint32_t KHR_LBVH_MAX_NODES = (2 * KHR_LBVH_MAX_LEAVES - 1);
constexpr uint32_t KHR_LBVH_QUERY_STACK_DEPTH = 64;

/*
 * Axis-Aligned Bounding Box (AABB) in 3D Euclidean space.
 */
typedef struct {
    float min[3];
    float max[3];
} khr_aabb_t;

/*
 * Bounded spatial element indexed along the Morton space-filling curve.
 */
typedef struct {
    uint32_t   id;          /* Entity / Instance ID */
    uint32_t   morton_code; /* 30-bit Morton code (Z-order key) */
    khr_aabb_t bounds;      /* World-space AABB */
} khr_spatial_item_t;

/*
 * Node in the Linear Bounding Volume Hierarchy binary radix tree.
 */
typedef struct {
    khr_aabb_t bounds;
    int32_t    left;       /* Left child node index */
    int32_t    right;      /* Right child node index */
    int32_t    parent;     /* Parent node index (-1 for root) */
    uint32_t   object_id;  /* Valid if is_leaf is true */
    bool       is_leaf;
} khr_lbvh_node_t;

/*
 * Linear Bounding Volume Hierarchy Accelerator (Pillar 2).
 * Built with zero dynamic heap allocations in O(N) time via Radix sort.
 */
typedef struct {
    khr_spatial_item_t leaves[KHR_LBVH_MAX_LEAVES];
    khr_lbvh_node_t    nodes[KHR_LBVH_MAX_NODES];
    uint32_t           leaf_count;
    uint32_t           node_count;
    int32_t            root;
    khr_aabb_t         scene_bounds;
} khr_lbvh_t;

/*
 * AABB Utility Functions
 */
[[nodiscard]]
static inline khr_aabb_t khr_aabb_make_empty(void) {
    return (khr_aabb_t){
        .min = { 1.0e30f, 1.0e30f, 1.0e30f },
        .max = { -1.0e30f, -1.0e30f, -1.0e30f },
    };
}

[[nodiscard]]
static inline khr_aabb_t khr_aabb_merge(khr_aabb_t a, khr_aabb_t b) {
    khr_aabb_t r;
    r.min[0] = (a.min[0] < b.min[0]) ? a.min[0] : b.min[0];
    r.min[1] = (a.min[1] < b.min[1]) ? a.min[1] : b.min[1];
    r.min[2] = (a.min[2] < b.min[2]) ? a.min[2] : b.min[2];
    r.max[0] = (a.max[0] > b.max[0]) ? a.max[0] : b.max[0];
    r.max[1] = (a.max[1] > b.max[1]) ? a.max[1] : b.max[1];
    r.max[2] = (a.max[2] > b.max[2]) ? a.max[2] : b.max[2];
    return r;
}

[[nodiscard]]
static inline bool khr_aabb_intersects(khr_aabb_t a, khr_aabb_t b) {
    if (a.max[0] < b.min[0] || a.min[0] > b.max[0]) return false;
    if (a.max[1] < b.min[1] || a.min[1] > b.max[1]) return false;
    if (a.max[2] < b.min[2] || a.min[2] > b.max[2]) return false;
    return true;
}

[[nodiscard]]
static inline bool khr_aabb_contains_point(khr_aabb_t a, const float p[3]) {
    return (p[0] >= a.min[0] && p[0] <= a.max[0] &&
            p[1] >= a.min[1] && p[1] <= a.max[1] &&
            p[2] >= a.min[2] && p[2] <= a.max[2]);
}

/*
 * Bitwise expansion: spreads 10-bit integer across 30 bits.
 * i.e. bit 0 -> bit 0, bit 1 -> bit 3, bit 2 -> bit 6...
 */
[[nodiscard]]
uint32_t khr_morton3d_expand(uint32_t v);

/*
 * Encode 3D Cartesian coordinates inside a normalized bounding domain
 * into a 30-bit Morton code (Z-order curve).
 */
[[nodiscard]]
uint32_t khr_morton3d_encode(float x, float y, float z, const khr_aabb_t* domain);

/*
 * Fast 4-pass 8-bit Least Significant Digit (LSD) Radix Sort on Morton keys.
 * Runs in strict O(N) time with zero dynamic heap allocation.
 */
void khr_radix_sort_morton(khr_spatial_item_t* items, uint32_t count);

/*
 * Build the Linear Bounding Volume Hierarchy from an array of spatial items.
 * Uses Karras (2012) binary radix tree hierarchy generation.
 */
[[nodiscard]]
bool khr_lbvh_build(khr_lbvh_t* bvh, const khr_spatial_item_t* items, uint32_t count);

/*
 * Query all objects in the LBVH overlapping a sphere (center + radius).
 * Returns number of matching object IDs written into out_ids (capped at max_results).
 */
uint32_t khr_lbvh_query_sphere(const khr_lbvh_t* bvh,
                               const float center[3],
                               float radius,
                               uint32_t* out_ids,
                               uint32_t max_results);

/*
 * Query all objects in the LBVH overlapping a query AABB.
 */
uint32_t khr_lbvh_query_aabb(const khr_lbvh_t* bvh,
                             const khr_aabb_t* query_box,
                             uint32_t* out_ids,
                             uint32_t max_results);

#endif /* KHOROS_CORE_SPATIAL_H */

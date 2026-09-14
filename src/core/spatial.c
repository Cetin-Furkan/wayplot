#include "khoros/core/spatial.h"
#include <string.h>
#include <math.h>

[[nodiscard]]
uint32_t khr_morton3d_expand(uint32_t v) {
    v &= 0x000003FFU; /* Mask to lowest 10 bits */
    v = (v | (v << 16)) & 0x030000FFU;
    v = (v | (v <<  8)) & 0x0300F00FU;
    v = (v | (v <<  4)) & 0x030C30C3U;
    v = (v | (v <<  2)) & 0x09249249U;
    return v;
}

[[nodiscard]]
uint32_t khr_morton3d_encode(float x, float y, float z, const khr_aabb_t* domain) {
    if (domain == nullptr) {
        return 0;
    }

    float dx = domain->max[0] - domain->min[0];
    float dy = domain->max[1] - domain->min[1];
    float dz = domain->max[2] - domain->min[2];

    float nx = (dx > 1e-6f) ? ((x - domain->min[0]) / dx) : 0.0f;
    float ny = (dy > 1e-6f) ? ((y - domain->min[1]) / dy) : 0.0f;
    float nz = (dz > 1e-6f) ? ((z - domain->min[2]) / dz) : 0.0f;

    nx = (nx < 0.0f) ? 0.0f : ((nx > 1.0f) ? 1.0f : nx);
    ny = (ny < 0.0f) ? 0.0f : ((ny > 1.0f) ? 1.0f : ny);
    nz = (nz < 0.0f) ? 0.0f : ((nz > 1.0f) ? 1.0f : nz);

    uint32_t ix = (uint32_t)(nx * 1023.0f);
    uint32_t iy = (uint32_t)(ny * 1023.0f);
    uint32_t iz = (uint32_t)(nz * 1023.0f);

    return (khr_morton3d_expand(ix)) |
           (khr_morton3d_expand(iy) << 1) |
           (khr_morton3d_expand(iz) << 2);
}

void khr_radix_sort_morton(khr_spatial_item_t* items, uint32_t count) {
    if (items == nullptr || count <= 1) {
        return;
    }

    khr_spatial_item_t temp[KHR_LBVH_MAX_LEAVES];
    khr_spatial_item_t* src = items;
    khr_spatial_item_t* dst = temp;

    /* 4 passes of 8-bit Radix Sort */
    for (uint32_t pass = 0; pass < 4; pass++) {
        uint32_t shift = pass * 8U;
        uint32_t count_table[256] = {};

        for (uint32_t i = 0; i < count; i++) {
            uint32_t bucket = (src[i].morton_code >> shift) & 0xFFU;
            count_table[bucket]++;
        }

        uint32_t prefix_table[256];
        prefix_table[0] = 0;
        for (uint32_t i = 1; i < 256; i++) {
            prefix_table[i] = prefix_table[i - 1] + count_table[i - 1];
        }

        for (uint32_t i = 0; i < count; i++) {
            uint32_t bucket = (src[i].morton_code >> shift) & 0xFFU;
            uint32_t dest_index = prefix_table[bucket]++;
            dst[dest_index] = src[i];
        }

        khr_spatial_item_t* swap = src;
        src = dst;
        dst = swap;
    }

    /* If sorted result landed in temp, copy back to items */
    if (src != items) {
        memcpy(items, src, (size_t)count * sizeof(khr_spatial_item_t));
    }
}

/* Find split index between range [first, last] using binary prefix difference */
[[nodiscard]]
static uint32_t khr_lbvh_find_split(const khr_spatial_item_t* items, uint32_t first, uint32_t last) {
    uint32_t first_code = items[first].morton_code;
    uint32_t last_code  = items[last].morton_code;

    if (first_code == last_code) {
        return (first + last) >> 1;
    }

    /* Common prefix length in bits (using leading zeros of XOR) */
    uint32_t common_prefix = (uint32_t)__builtin_clz(first_code ^ last_code);

    /* Binary search for the split point where prefix length changes */
    uint32_t split = first;
    uint32_t step  = last - first;

    while (step > 1) {
        step = (step + 1) >> 1;
        uint32_t mid = split + step;
        if (mid < last) {
            uint32_t mid_code = items[mid].morton_code;
            uint32_t mid_prefix = (uint32_t)__builtin_clz(first_code ^ mid_code);
            if (mid_prefix > common_prefix) {
                split = mid;
            }
        }
    }
    return split;
}

static int32_t khr_lbvh_build_recursive(khr_lbvh_t* bvh,
                                        const khr_spatial_item_t* items,
                                        uint32_t first,
                                        uint32_t last) {
    if (first > last || bvh->node_count >= KHR_LBVH_MAX_NODES) {
        return -1;
    }

    int32_t node_idx = (int32_t)bvh->node_count++;
    khr_lbvh_node_t* node = &bvh->nodes[node_idx];
    node->parent = -1;

    if (first == last) {
        /* Leaf node */
        node->is_leaf = true;
        node->object_id = items[first].id;
        node->left = -1;
        node->right = -1;
        node->bounds = items[first].bounds;
        return node_idx;
    }

    /* Internal node: split range */
    node->is_leaf = false;
    node->object_id = UINT32_MAX;

    uint32_t split = khr_lbvh_find_split(items, first, last);

    int32_t left_child = khr_lbvh_build_recursive(bvh, items, first, split);
    int32_t right_child = khr_lbvh_build_recursive(bvh, items, split + 1, last);

    node->left = left_child;
    node->right = right_child;

    if (left_child >= 0) {
        bvh->nodes[left_child].parent = node_idx;
    }
    if (right_child >= 0) {
        bvh->nodes[right_child].parent = node_idx;
    }

    khr_aabb_t left_box = (left_child >= 0) ? bvh->nodes[left_child].bounds : khr_aabb_make_empty();
    khr_aabb_t right_box = (right_child >= 0) ? bvh->nodes[right_child].bounds : khr_aabb_make_empty();
    node->bounds = khr_aabb_merge(left_box, right_box);

    return node_idx;
}

[[nodiscard]]
bool khr_lbvh_build(khr_lbvh_t* bvh, const khr_spatial_item_t* items, uint32_t count) {
    if (bvh == nullptr) {
        return false;
    }
    memset(bvh, 0, sizeof(*bvh));

    if (items == nullptr || count == 0) {
        bvh->root = -1;
        return true;
    }
    if (count > KHR_LBVH_MAX_LEAVES) {
        count = KHR_LBVH_MAX_LEAVES;
    }

    bvh->leaf_count = count;
    memcpy(bvh->leaves, items, (size_t)count * sizeof(khr_spatial_item_t));

    /* 1. Calculate overall scene bounds */
    khr_aabb_t scene_box = khr_aabb_make_empty();
    for (uint32_t i = 0; i < count; i++) {
        scene_box = khr_aabb_merge(scene_box, bvh->leaves[i].bounds);
    }
    bvh->scene_bounds = scene_box;

    /* 2. Compute Morton codes for each element */
    for (uint32_t i = 0; i < count; i++) {
        float cx = 0.5f * (bvh->leaves[i].bounds.min[0] + bvh->leaves[i].bounds.max[0]);
        float cy = 0.5f * (bvh->leaves[i].bounds.min[1] + bvh->leaves[i].bounds.max[1]);
        float cz = 0.5f * (bvh->leaves[i].bounds.min[2] + bvh->leaves[i].bounds.max[2]);
        bvh->leaves[i].morton_code = khr_morton3d_encode(cx, cy, cz, &bvh->scene_bounds);
    }

    /* 3. Radix Sort items along Morton space-filling curve */
    khr_radix_sort_morton(bvh->leaves, count);

    /* 4. Recursively build binary hierarchy */
    bvh->node_count = 0;
    bvh->root = khr_lbvh_build_recursive(bvh, bvh->leaves, 0, count - 1);

    return (bvh->root >= 0);
}

[[nodiscard]]
static inline bool khr_sphere_intersects_aabb(const float center[3], float radius, khr_aabb_t box) {
    float r2 = radius * radius;
    float dist2 = 0.0f;

    for (int i = 0; i < 3; i++) {
        float v = center[i];
        if (v < box.min[i]) {
            float d = box.min[i] - v;
            dist2 += d * d;
        } else if (v > box.max[i]) {
            float d = v - box.max[i];
            dist2 += d * d;
        }
    }
    return dist2 <= r2;
}

uint32_t khr_lbvh_query_sphere(const khr_lbvh_t* bvh,
                               const float center[3],
                               float radius,
                               uint32_t* out_ids,
                               uint32_t max_results) {
    if (bvh == nullptr || bvh->root < 0 || out_ids == nullptr || max_results == 0) {
        return 0;
    }

    int32_t stack[KHR_LBVH_QUERY_STACK_DEPTH];
    uint32_t sp = 0;
    stack[sp++] = bvh->root;

    uint32_t result_count = 0;

    while (sp > 0) {
        int32_t node_idx = stack[--sp];
        if (node_idx < 0 || (uint32_t)node_idx >= bvh->node_count) {
            continue;
        }
        const khr_lbvh_node_t* node = &bvh->nodes[node_idx];

        if (!khr_sphere_intersects_aabb(center, radius, node->bounds)) {
            continue;
        }

        if (node->is_leaf) {
            if (result_count < max_results) {
                out_ids[result_count++] = node->object_id;
            }
        } else {
            if (sp + 2 <= KHR_LBVH_QUERY_STACK_DEPTH) {
                if (node->right >= 0) stack[sp++] = node->right;
                if (node->left >= 0)  stack[sp++] = node->left;
            }
        }
    }
    return result_count;
}

uint32_t khr_lbvh_query_aabb(const khr_lbvh_t* bvh,
                             const khr_aabb_t* query_box,
                             uint32_t* out_ids,
                             uint32_t max_results) {
    if (bvh == nullptr || bvh->root < 0 || query_box == nullptr || out_ids == nullptr || max_results == 0) {
        return 0;
    }

    int32_t stack[KHR_LBVH_QUERY_STACK_DEPTH];
    uint32_t sp = 0;
    stack[sp++] = bvh->root;

    uint32_t result_count = 0;

    while (sp > 0) {
        int32_t node_idx = stack[--sp];
        if (node_idx < 0 || (uint32_t)node_idx >= bvh->node_count) {
            continue;
        }
        const khr_lbvh_node_t* node = &bvh->nodes[node_idx];

        if (!khr_aabb_intersects(*query_box, node->bounds)) {
            continue;
        }

        if (node->is_leaf) {
            if (result_count < max_results) {
                out_ids[result_count++] = node->object_id;
            }
        } else {
            if (sp + 2 <= KHR_LBVH_QUERY_STACK_DEPTH) {
                if (node->right >= 0) stack[sp++] = node->right;
                if (node->left >= 0)  stack[sp++] = node->left;
            }
        }
    }
    return result_count;
}

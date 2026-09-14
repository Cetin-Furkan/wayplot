#include "test_framework.h"
#include "khoros/core/spatial.h"
#include <stdlib.h>
#include <stdio.h>

static float randf(float min, float max) {
    float r = (float)rand() / (float)RAND_MAX;
    return min + r * (max - min);
}

[[nodiscard]]
bool test_lbvh_vs_naive_broadphase(void) {
    const uint32_t test_sizes[] = { 64, 256 };

    for (size_t s = 0; s < sizeof(test_sizes) / sizeof(test_sizes[0]); s++) {
        uint32_t N = test_sizes[s];
        srand(1337 + N);

        khr_spatial_item_t items[KHR_LBVH_MAX_LEAVES];
        khr_aabb_t domain = khr_aabb_make_empty();

        for (uint32_t i = 0; i < N; i++) {
            float cx = randf(-10.0f, 10.0f);
            float cy = randf(-10.0f, 10.0f);
            float cz = randf(-10.0f, 10.0f);

            float hx = randf(0.2f, 1.5f);
            float hy = randf(0.2f, 1.5f);
            float hz = randf(0.2f, 1.5f);

            items[i].id = i;
            items[i].bounds = (khr_aabb_t){
                .min = { cx - hx, cy - hy, cz - hz },
                .max = { cx + hx, cy + hy, cz + hz },
            };
            domain = khr_aabb_merge(domain, items[i].bounds);
        }

        /* Morton code computation and Radix sort */
        for (uint32_t i = 0; i < N; i++) {
            float mid_x = 0.5f * (items[i].bounds.min[0] + items[i].bounds.max[0]);
            float mid_y = 0.5f * (items[i].bounds.min[1] + items[i].bounds.max[1]);
            float mid_z = 0.5f * (items[i].bounds.min[2] + items[i].bounds.max[2]);
            items[i].morton_code = khr_morton3d_encode(mid_x, mid_y, mid_z, &domain);
        }

        khr_radix_sort_morton(items, N);

        khr_lbvh_t bvh = {};
        TEST_ASSERT(khr_lbvh_build(&bvh, items, N), "LBVH build failed");

        /* Compute all O(N^2) naive overlaps and verify 0 false negatives in LBVH */
        uint32_t total_naive_overlaps = 0;
        uint32_t verified_lbvh_matches = 0;

        for (uint32_t i = 0; i < N; i++) {
            uint32_t candidate_ids[KHR_LBVH_MAX_LEAVES];
            uint32_t count = khr_lbvh_query_aabb(&bvh, &items[i].bounds, candidate_ids, KHR_LBVH_MAX_LEAVES);

            for (uint32_t j = 0; j < N; j++) {
                bool naive_overlap = khr_aabb_intersects(items[i].bounds, items[j].bounds);
                if (naive_overlap) {
                    total_naive_overlaps++;

                    /* Find items[j].id in candidate_ids */
                    bool found_in_lbvh = false;
                    for (uint32_t c = 0; c < count; c++) {
                        if (candidate_ids[c] == items[j].id) {
                            found_in_lbvh = true;
                            break;
                        }
                    }

                    TEST_ASSERT(found_in_lbvh, "LBVH must have ZERO false negatives (all true overlaps found)");
                    verified_lbvh_matches++;
                }
            }
        }

        printf("    [LBVH vs Naive N=%u] Overlaps=%u | 0 False Negatives (100%% recall)\n",
               N, total_naive_overlaps);
        TEST_ASSERT_EQ(verified_lbvh_matches, total_naive_overlaps,
                       "All naive overlaps must be present in LBVH candidate queries");
    }

    return true;
}

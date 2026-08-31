#ifndef WAYLAND_FEEDBACK_H
#define WAYLAND_FEEDBACK_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

/* zwp_linux_dmabuf_feedback_v1 snapshot. No Wayland object ids.
 * Vulkan may include this header: it is DRM fourcc + modifiers + dev_t. */

#define WP_TRANCHE_SCANOUT 1u

struct wp_format_table_entry {
    uint32_t format;
    uint32_t pad;
    uint64_t modifier;
};

struct wp_format_pair {
    uint32_t format;
    uint64_t modifier;
};

struct wp_tranche {
    dev_t target_device;
    uint32_t flags;
    struct wp_format_pair *pairs;
    uint32_t pair_count;
};

struct wp_feedback {
    dev_t main_device;
    struct wp_format_table_entry *table;
    uint32_t table_count;
    struct wp_tranche *tranches;
    uint32_t ntranches;
    uint32_t cap;
    int current;
    bool done;
};

void wp_feedback_init(struct wp_feedback *f);
void wp_feedback_reset(struct wp_feedback *f);
void wp_feedback_free(struct wp_feedback *f);

[[nodiscard]] int wp_feedback_set_table(struct wp_feedback *f, int fd, uint32_t bytes);
void wp_feedback_set_main_device(struct wp_feedback *f, dev_t dev);
[[nodiscard]] struct wp_tranche *wp_feedback_cur(struct wp_feedback *f);
[[nodiscard]] int wp_feedback_add_indices(struct wp_feedback *f, const uint16_t *idx, uint32_t n);
void wp_feedback_tranche_done(struct wp_feedback *f);
void wp_feedback_print(const struct wp_feedback *f, FILE *out);

#endif /* WAYLAND_FEEDBACK_H */

#ifndef VULKAN_NEGOTIATE_H
#define VULKAN_NEGOTIATE_H

#include "wayland/feedback.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <vulkan/vulkan.h>

struct wp_vk_mod_list {
    VkFormat vk_format;
    uint32_t drm_format;
    uint64_t *modifiers;
    uint32_t modifier_count;
};

struct wp_vk_formats {
    struct wp_vk_mod_list *v;
    uint32_t n;
    uint32_t skipped_disjoint;
};

struct wp_negotiated {
    uint32_t drm_format;
    VkFormat vk_format;
    uint64_t *modifiers;
    uint32_t modifier_count;
    uint64_t chosen_modifier;
    bool scanout;
    dev_t target_device;
    bool valid;
};

void wp_vk_formats_free(struct wp_vk_formats *f);
void wp_negotiated_free(struct wp_negotiated *n);

[[nodiscard]] int wp_vk_query_formats(VkPhysicalDevice phy, struct wp_vk_formats *out);
[[nodiscard]] int wp_negotiate(const struct wp_feedback *fb, const struct wp_vk_formats *vk,
                               dev_t render, dev_t primary, struct wp_negotiated *out);
void wp_vk_formats_print(const struct wp_vk_formats *f, FILE *out);
void wp_negotiated_print(const struct wp_negotiated *n, FILE *out);

#endif /* VULKAN_NEGOTIATE_H */

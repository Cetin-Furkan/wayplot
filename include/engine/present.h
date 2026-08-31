#ifndef ENGINE_PRESENT_H
#define ENGINE_PRESENT_H

#include "vulkan/device.h"
#include "vulkan/negotiate.h"
#include "vulkan/swapchain.h"
#include "wayland/session.h"

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

struct wp_present_frame {
    VkCommandBuffer cmd; /* open: begin recorded the color barrier; caller draws; end submits */
    VkImage image;
    VkImageView view;
    VkExtent2D extent; /* buffer pixels = logical × scale */
    int32_t scale;
    int32_t logical_width;
    int32_t logical_height;
    uint32_t toplevel_states;
    uint32_t slot;
};

struct wp_present {
    struct wp_session *session;
    struct wp_device device;
    struct wp_vk_formats formats;
    struct wp_negotiated negotiated;
    struct wp_swapchain sc;
    uint32_t wl_buffer[WP_SWAPCHAIN_IMAGES];
    uint32_t wl_release_timeline[WP_SWAPCHAIN_IMAGES];
    uint32_t acquire_timeline;
    bool sync_setup;
    float clear[4];
    int wait_efd[WP_SWAPCHAIN_IMAGES];
};

[[nodiscard]] int wp_present_open(struct wp_present *p, struct wp_session *s);
void wp_present_close(struct wp_present *p);
[[nodiscard]] int wp_present_poll(struct wp_present *p, uint64_t timeout_ns);
[[nodiscard]] bool wp_present_begin(struct wp_present *p, struct wp_present_frame *f);
[[nodiscard]] int wp_present_end(struct wp_present *p, const struct wp_present_frame *f);

#endif /* ENGINE_PRESENT_H */

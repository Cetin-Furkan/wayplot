#define _GNU_SOURCE
#include "engine/present.h"

#include "helper/drmfd.h"
#include "helper/log.h"

#include <vulkan/vulkan.h>

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

static int export_all(struct wp_present *p)
{
    uint32_t i;
    int ret;

    for (i = 0; i < WP_SWAPCHAIN_IMAGES; i++) {
        struct wp_vk_image *img = &p->sc.images[i];
        struct wp_wl_export e = {
            .dma_fd = img->dma_fd,
            .plane_count = img->plane_count,
            .modifier = img->modifier,
            .drm_format = p->negotiated.drm_format,
            .width = (int32_t)p->sc.width,
            .height = (int32_t)p->sc.height,
        };
        uint32_t pl;

        for (pl = 0; pl < img->plane_count; pl++) {
            e.offset[pl] = img->planes[pl].offset;
            e.stride[pl] = img->planes[pl].stride;
        }
        if (p->wl_buffer[i]) {
            (void)wp_session_destroy_wl_buffer(p->session, p->wl_buffer[i]);
            p->wl_buffer[i] = 0;
        }
        ret = wp_session_create_wl_buffer(p->session, &e, &p->wl_buffer[i]);
        if (ret < 0)
            return ret;
        if (p->wl_release_timeline[i] == 0) {
            ret = wp_session_import_timeline(p->session, img->release_fd,
                                             &p->wl_release_timeline[i]);
            if (ret < 0)
                return ret;
        }
    }
    return 0;
}

static void disarm_waits(struct wp_present *p)
{
    uint32_t i;
    for (i = 0; i < WP_SWAPCHAIN_IMAGES; i++) {
        if (p->wait_efd[i] >= 0) {
            close(p->wait_efd[i]);
            p->wait_efd[i] = -1;
        }
    }
}

static int recreate(struct wp_present *p, uint32_t w, uint32_t h)
{
    uint32_t i;
    int ret;

    /* In-flight DMA-BUFs must be idle before we free them. */
    if (p->device.device)
        (void)vkDeviceWaitIdle(p->device.device);
    disarm_waits(p);
    wp_swapchain_retire(&p->sc);
    for (i = 0; i < WP_SWAPCHAIN_IMAGES; i++) {
        if (p->wl_buffer[i]) {
            (void)wp_session_destroy_wl_buffer(p->session, p->wl_buffer[i]);
            p->wl_buffer[i] = 0;
        }
        p->wl_release_timeline[i] = 0;
    }
    wp_swapchain_free_retired(&p->device, &p->sc);
    ret = wp_swapchain_create(&p->device, &p->sc, w, h, &p->negotiated);
    if (ret < 0)
        return ret;
    return export_all(p);
}

int wp_present_open(struct wp_present *p, struct wp_session *s)
{
    int ret;

    if (!p || !s)
        return -EINVAL;
    memset(p, 0, sizeof(*p));
    p->session = s;
    p->clear[0] = 0.07f;
    p->clear[1] = 0.16f;
    p->clear[2] = 0.22f;
    p->clear[3] = 1.0f;
    wp_swapchain_init(&p->sc);
    for (uint32_t i = 0; i < WP_SWAPCHAIN_IMAGES; i++)
        p->wait_efd[i] = -1;

    ret = wp_device_open(&p->device, s->fb.main_device);
    if (ret < 0)
        return ret;
    ret = wp_vk_query_formats(p->device.phy, &p->formats);
    if (ret < 0) {
        wp_present_close(p);
        return ret;
    }
    ret = wp_negotiate(&s->fb, &p->formats, p->device.render, p->device.primary,
                       &p->negotiated);
    if (ret < 0) {
        fprintf(stderr, "no compositor∩GPU format/modifier\n");
        wp_vk_formats_print(&p->formats, stderr);
        wp_feedback_print(&s->fb, stderr);
        wp_present_close(p);
        return ret;
    }
    {
        uint32_t bw, bh;
        wp_session_buffer_size(s, &bw, &bh);
        ret = wp_swapchain_create(&p->device, &p->sc, bw, bh, &p->negotiated);
    }
    if (ret < 0) {
        wp_present_close(p);
        return ret;
    }
    ret = wp_session_setup_explicit_sync(s, p->device.acquire_fd);
    if (ret < 0) {
        wp_present_close(p);
        return ret;
    }
    p->acquire_timeline = s->acquire_timeline;
    p->sync_setup = true;
    ret = export_all(p);
    if (ret < 0) {
        wp_present_close(p);
        return ret;
    }
    p->negotiated.chosen_modifier = p->sc.params.chosen_modifier;
    wp_device_print(&p->device, stdout);
    wp_vk_formats_print(&p->formats, stdout);
    wp_negotiated_print(&p->negotiated, stdout);
    return 0;
}

void wp_present_close(struct wp_present *p)
{
    uint32_t i;

    if (!p)
        return;
    for (i = 0; i < WP_SWAPCHAIN_IMAGES; i++) {
        if (p->wait_efd[i] >= 0) {
            close(p->wait_efd[i]);
            p->wait_efd[i] = -1;
        }
    }
    if (p->session) {
        for (i = 0; i < WP_SWAPCHAIN_IMAGES; i++) {
            if (p->wl_buffer[i])
                (void)wp_session_destroy_wl_buffer(p->session, p->wl_buffer[i]);
        }
    }
    if (p->sc.allocated)
        wp_swapchain_retire(&p->sc);
    for (i = 0; i < p->sc.retired_count; i++)
        wp_swapchain_destroy_image(&p->device, &p->sc.retired[i]);
    free(p->sc.retired);
    p->sc.retired = NULL;
    wp_negotiated_free(&p->sc.params);
    wp_negotiated_free(&p->negotiated);
    wp_vk_formats_free(&p->formats);
    wp_device_close(&p->device);
    memset(p, 0, sizeof(*p));
}

int wp_present_poll(struct wp_present *p, uint64_t timeout_ns)
{
    uint32_t i;

    if (!p || !p->session)
        return -EINVAL;
    wp_swapchain_free_retired(&p->device, &p->sc);
    {
        int ret = wp_session_pump(p->session, timeout_ns);
        if (ret < 0)
            return ret;
    }
    for (i = 0; i < WP_SWAPCHAIN_IMAGES; i++) {
        if ((p->session->conn.poll_ready & (1u << i)) && p->wait_efd[i] >= 0) {
            uint64_t val;
            (void)read(p->wait_efd[i], &val, sizeof(val));
            close(p->wait_efd[i]);
            p->wait_efd[i] = -1;
        }
    }
    return 0;
}

static int arm_release_waits(struct wp_present *p)
{
    uint32_t i;
    int ret;

    p->session->conn.poll_ready = 0;
    for (i = 0; i < WP_SWAPCHAIN_IMAGES; i++) {
        struct wp_vk_image *img = &p->sc.images[i];
        int efd;

        if (img->last_release == 0)
            continue;
        if (p->wait_efd[i] >= 0)
            continue;
        efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (efd < 0)
            return -errno;
        ret = wp_drm_syncobj_eventfd(p->device.drm_fd, img->drm_handle, img->last_release, efd);
        if (ret < 0) {
            close(efd);
            return ret;
        }
        ret = wp_wl_poll_add(&p->session->conn, efd, POLLIN, i);
        if (ret < 0) {
            close(efd);
            return ret;
        }
        p->wait_efd[i] = efd;
    }
    return 0;
}

static int begin_cmd(struct wp_vk_image *img)
{
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VkImageMemoryBarrier2 b1 = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img->image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    VkDependencyInfo dep1 = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &b1,
    };

    if (vkBeginCommandBuffer(img->cmd, &bi) != VK_SUCCESS)
        return -EIO;
    vkCmdPipelineBarrier2(img->cmd, &dep1);
    return 0;
}

bool wp_present_begin(struct wp_present *p, struct wp_present_frame *f)
{
    uint32_t slot;
    struct wp_vk_image *img;
    int ret;

    if (!p || !f || !p->sc.allocated)
        return false;
    if (p->session->closed)
        return false;
    if (p->session->size_dirty) {
        uint32_t w, h;
        wp_session_buffer_size(p->session, &w, &h);
        if (w != p->sc.width || h != p->sc.height) {
            if (recreate(p, w, h) < 0)
                return false;
        }
        p->session->size_dirty = false;
    }
    if (p->session->configure_dirty) {
        (void)wp_session_ack(p->session);
        p->session->configure_dirty = false;
    }
    if (!p->session->frame_done)
        return false;
    ret = wp_swapchain_pick(&p->device, &p->sc, &slot);
    if (ret == -EAGAIN) {
        (void)arm_release_waits(p);
        return false;
    }
    if (ret < 0)
        return false;
    img = &p->sc.images[slot];
    if (wp_swapchain_wait_gpu(&p->device, img) < 0)
        return false;
    vkResetCommandBuffer(img->cmd, 0);
    if (begin_cmd(img) < 0)
        return false;
    f->cmd = img->cmd;
    f->image = img->image;
    f->view = img->view;
    f->extent = (VkExtent2D){ p->sc.width, p->sc.height };
    f->scale = p->session->scale > 0 ? p->session->scale : 1;
    f->logical_width = p->session->width;
    f->logical_height = p->session->height;
    f->toplevel_states = p->session->toplevel_states;
    f->slot = slot;
    p->sc.cursor = slot;
    return true;
}

int wp_present_end(struct wp_present *p, const struct wp_present_frame *f)
{
    struct wp_vk_image *img;
    uint64_t acq, rel;
    VkSemaphoreSubmitInfo wait_si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
    VkSemaphoreSubmitInfo sig_si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
    VkCommandBufferSubmitInfo cmd_si = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
    VkSubmitInfo2 sub = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    bool has_wait;
    int ret;

    if (!p || !f || f->slot >= WP_SWAPCHAIN_IMAGES)
        return -EINVAL;
    img = &p->sc.images[f->slot];
    if (vkEndCommandBuffer(img->cmd) != VK_SUCCESS)
        return -EIO;
    acq = ++p->device.acquire_point;
    rel = img->last_release + 1;
    has_wait = img->last_release != 0;
    if (has_wait) {
        wait_si.semaphore = img->release_sem;
        wait_si.value = img->last_release;
        wait_si.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    }
    sig_si.semaphore = p->device.acquire_sem;
    sig_si.value = acq;
    sig_si.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    cmd_si.commandBuffer = img->cmd;
    sub.waitSemaphoreInfoCount = has_wait ? 1u : 0u;
    sub.pWaitSemaphoreInfos = has_wait ? &wait_si : NULL;
    sub.commandBufferInfoCount = 1;
    sub.pCommandBufferInfos = &cmd_si;
    sub.signalSemaphoreInfoCount = 1;
    sub.pSignalSemaphoreInfos = &sig_si;
    if (vkQueueSubmit2(p->device.gfx, 1, &sub, VK_NULL_HANDLE) != VK_SUCCESS)
        return -EIO;
    img->last_acquire = acq;
    img->last_release = rel;

    ret = wp_session_commit_buffer(p->session, p->wl_buffer[f->slot],
                                   p->acquire_timeline, acq,
                                   p->wl_release_timeline[f->slot], rel);
    if (ret < 0)
        return ret;
    p->sc.cursor = (f->slot + 1) % WP_SWAPCHAIN_IMAGES;
    return 0;
}

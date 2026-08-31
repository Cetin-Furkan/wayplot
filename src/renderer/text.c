#define _GNU_SOURCE
#include "renderer/text.h"

#include "helper/math3d.h"
#include "renderer/camera.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct text_ubo {
    float vp[4];
    float color[4];
};

/* Vulkan max minUniformBufferOffsetAlignment. */
#define TEXT_UBO_STRIDE 256

static const unsigned char text_vert_spv[] __attribute__((aligned(4))) = {
#embed "shaders/text.vert.spv"
};
static const unsigned char text_frag_spv[] __attribute__((aligned(4))) = {
#embed "shaders/text.frag.spv"
};

static uint32_t utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t c;

    if (!p || !*p)
        return 0;
    c = p[0];
    if (c < 0x80u) {
        *s = (const char *)(p + 1);
        return c;
    }
    if ((c & 0xe0u) == 0xc0u && (p[1] & 0xc0u) == 0x80u) {
        *s = (const char *)(p + 2);
        return ((c & 0x1fu) << 6) | (p[1] & 0x3fu);
    }
    if ((c & 0xf0u) == 0xe0u && (p[1] & 0xc0u) == 0x80u && (p[2] & 0xc0u) == 0x80u) {
        *s = (const char *)(p + 3);
        return ((c & 0x0fu) << 12) | ((p[1] & 0x3fu) << 6) | (p[2] & 0x3fu);
    }
    if ((c & 0xf8u) == 0xf0u && (p[1] & 0xc0u) == 0x80u && (p[2] & 0xc0u) == 0x80u &&
        (p[3] & 0xc0u) == 0x80u) {
        *s = (const char *)(p + 4);
        return ((c & 0x07u) << 18) | ((p[1] & 0x3fu) << 12) | ((p[2] & 0x3fu) << 6) | (p[3] & 0x3fu);
    }
    *s = (const char *)(p + 1);
    return 0xfffdu;
}

void wp_text_geom_free(struct wp_text_geom *g)
{
    if (!g)
        return;
    free(g->v);
    free(g->idx);
    memset(g, 0, sizeof(*g));
}

int wp_text_layout(const struct wp_font *f, const char *utf8, float origin_x, float origin_y,
                   struct wp_text_geom *g)
{
    const char *p;
    uint32_t prev = 0;
    float pen_x, pen_y, x0, y0, x1, y1;
    uint32_t cap = WP_TEXT_MAX_GLYPHS;
    uint32_t n = 0;

    if (!f || !g)
        return -EINVAL;
    memset(g, 0, sizeof(*g));
    if (!utf8)
        utf8 = "";
    g->v = calloc(cap * 4, sizeof(*g->v));
    g->idx = calloc(cap * 6, sizeof(*g->idx));
    if (!g->v || !g->idx) {
        wp_text_geom_free(g);
        return -ENOMEM;
    }
    pen_x = origin_x;
    pen_y = origin_y + f->ascent;
    x0 = origin_x;
    y0 = origin_y;
    x1 = origin_x;
    y1 = origin_y;
    p = utf8;
    while (*p && n < cap) {
        uint32_t cp = utf8_next(&p);
        const struct wp_glyph *gl;
        float gx, gy;
        uint16_t base;

        if (cp == 0)
            break;
        if (cp == (uint32_t)'\n') {
            pen_x = origin_x;
            pen_y += f->line_height;
            prev = 0;
            continue;
        }
        if (cp == (uint32_t)'\r') {
            prev = 0;
            continue;
        }
        if (cp == (uint32_t)'\t') {
            const struct wp_glyph *sp = wp_font_glyph(f, (uint32_t)' ');
            float adv = sp ? sp->advance * 4.0f : f->size_px * 2.0f;
            pen_x += adv;
            prev = 0;
            continue;
        }
        gl = wp_font_glyph(f, cp);
        if (!gl || !gl->present)
            continue;
        if (prev)
            pen_x += wp_font_kern(f, prev, cp);
        gx = pen_x + gl->left;
        gy = pen_y - gl->top;
        if (gl->w > 0.0f && gl->h > 0.0f) {
            /* TL, BL, BR, TR — front-facing under pixel ortho + CCW. */
            g->v[n * 4 + 0] = (struct wp_text_vertex){ gx, gy, gl->u0, gl->v0 };
            g->v[n * 4 + 1] = (struct wp_text_vertex){ gx, gy + gl->h, gl->u0, gl->v1 };
            g->v[n * 4 + 2] = (struct wp_text_vertex){ gx + gl->w, gy + gl->h, gl->u1, gl->v1 };
            g->v[n * 4 + 3] = (struct wp_text_vertex){ gx + gl->w, gy, gl->u1, gl->v0 };
            base = (uint16_t)(n * 4);
            g->idx[n * 6 + 0] = base;
            g->idx[n * 6 + 1] = (uint16_t)(base + 1);
            g->idx[n * 6 + 2] = (uint16_t)(base + 2);
            g->idx[n * 6 + 3] = base;
            g->idx[n * 6 + 4] = (uint16_t)(base + 2);
            g->idx[n * 6 + 5] = (uint16_t)(base + 3);
            if (gx < x0)
                x0 = gx;
            if (gy < y0)
                y0 = gy;
            if (gx + gl->w > x1)
                x1 = gx + gl->w;
            if (gy + gl->h > y1)
                y1 = gy + gl->h;
            n++;
        }
        pen_x += gl->advance;
        prev = cp;
    }
    if (n == 0) {
        free(g->v);
        free(g->idx);
        g->v = NULL;
        g->idx = NULL;
        g->x0 = origin_x;
        g->y0 = origin_y;
        g->x1 = origin_x;
        g->y1 = origin_y + f->line_height;
        return 0;
    }
    g->nv = n * 4;
    g->ni = n * 6;
    g->x0 = x0;
    g->y0 = y0;
    g->x1 = x1;
    g->y1 = y1;
    return 0;
}

static VkShaderModule make_module(VkDevice dev, const unsigned char *spv, size_t n)
{
    VkShaderModuleCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = n,
        .pCode = (const uint32_t *)spv,
    };
    VkShaderModule m = VK_NULL_HANDLE;
    if (n < 4 || (n & 3u) != 0)
        return VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &ci, NULL, &m) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return m;
}

static int make_pipeline(struct wp_text *t)
{
    VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = t->vs,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = t->fs,
            .pName = "main",
        },
    };
    VkVertexInputBindingDescription bind = {
        .binding = 0,
        .stride = sizeof(struct wp_text_vertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };
    VkVertexInputAttributeDescription attr[2] = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 0 },
        { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 8 },
    };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &bind,
        .vertexAttributeDescriptionCount = 2,
        .pVertexAttributeDescriptions = attr,
    };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_BACK_BIT,
        .frontFace = wp_camera_front_clockwise() ? VK_FRONT_FACE_CLOCKWISE
                                                 : VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineColorBlendAttachmentState ba = {
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &ba,
    };
    VkDynamicState dyns[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dyns,
    };
    VkPipelineRenderingCreateInfo rend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &t->color_fmt,
    };
    VkGraphicsPipelineCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rend,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vi,
        .pInputAssemblyState = &ia,
        .pViewportState = &vp,
        .pRasterizationState = &rs,
        .pMultisampleState = &ms,
        .pColorBlendState = &cb,
        .pDynamicState = &dyn,
        .layout = t->dev->push_layout,
    };
    if (vkCreateGraphicsPipelines(t->dev->device, VK_NULL_HANDLE, 1, &pci, NULL, &t->pipeline) != VK_SUCCESS)
        return -EIO;
    return 0;
}

int wp_text_init(struct wp_text *t, struct wp_device *d, VkFormat color_fmt)
{
    uint32_t i;
    int ret;
    VkDeviceSize vbytes = (VkDeviceSize)WP_TEXT_MAX_GLYPHS * 4 * sizeof(struct wp_text_vertex);
    VkDeviceSize ibytes = (VkDeviceSize)WP_TEXT_MAX_GLYPHS * 6 * sizeof(uint16_t);

    if (!t || !d)
        return -EINVAL;
    memset(t, 0, sizeof(*t));
    t->dev = d;
    t->color_fmt = color_fmt;
    for (i = 0; i < WP_SWAPCHAIN_IMAGES; i++) {
        ret = wp_buffer_create(d, (VkDeviceSize)WP_TEXT_MAX_DRAWS * TEXT_UBO_STRIDE,
                               VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               &t->ubo[i]);
        if (ret < 0)
            return ret;
        if (!t->ubo[i].mapped)
            return -ENOMEM;
        t->ubo_used[i] = 0;
        t->vused[i] = 0;
        t->iused[i] = 0;
        ret = wp_buffer_create(d, vbytes,
                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               &t->vbo[i]);
        if (ret < 0)
            return ret;
        if (!t->vbo[i].mapped)
            return -ENOMEM;
        ret = wp_buffer_create(d, ibytes,
                               VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               &t->ibo[i]);
        if (ret < 0)
            return ret;
        if (!t->ibo[i].mapped)
            return -ENOMEM;
    }
    t->vs = make_module(d->device, text_vert_spv, sizeof(text_vert_spv));
    t->fs = make_module(d->device, text_frag_spv, sizeof(text_frag_spv));
    if (!t->vs || !t->fs) {
        fprintf(stderr, "text: shader module failed vert=%zu frag=%zu\n",
                sizeof(text_vert_spv), sizeof(text_frag_spv));
        return -EIO;
    }
    ret = make_pipeline(t);
    if (ret < 0)
        fprintf(stderr, "text: pipeline failed\n");
    return ret;
}

void wp_text_reset(struct wp_text *t, uint32_t slot)
{
    if (!t || slot >= WP_SWAPCHAIN_IMAGES)
        return;
    t->ubo_used[slot] = 0;
    t->vused[slot] = 0;
    t->iused[slot] = 0;
}

void wp_text_draw(struct wp_text *t, VkCommandBuffer cmd, uint32_t width, uint32_t height,
                  uint32_t slot, const struct wp_font *font, const struct wp_text_geom *geom,
                  const float rgba[4])
{
    struct text_ubo u;
    VkDeviceSize voff = 0;
    VkDeviceSize uoff;
    VkDescriptorBufferInfo ubo_info;
    VkDescriptorImageInfo img_info;
    VkWriteDescriptorSet writes[2];
    uint32_t nv, ni, vbase, ibase, di;
    uint32_t vmax = WP_TEXT_MAX_GLYPHS * 4;
    uint32_t imax = WP_TEXT_MAX_GLYPHS * 6;

    if (!t || !cmd || !font || !geom || !rgba || slot >= WP_SWAPCHAIN_IMAGES)
        return;
    if (width == 0 || height == 0 || !font->view || geom->ni == 0)
        return;
    nv = geom->nv;
    ni = geom->ni;
    vbase = t->vused[slot];
    ibase = t->iused[slot];
    if (vbase > vmax || ibase > imax)
        return;
    if (nv > vmax - vbase || ni > imax - ibase)
        return;
    if (t->ubo_used[slot] >= WP_TEXT_MAX_DRAWS)
        return;
    di = t->ubo_used[slot];
    uoff = (VkDeviceSize)di * TEXT_UBO_STRIDE;
    memcpy((unsigned char *)t->vbo[slot].mapped + (size_t)vbase * sizeof(*geom->v),
           geom->v, (size_t)nv * sizeof(*geom->v));
    memcpy((unsigned char *)t->ibo[slot].mapped + (size_t)ibase * sizeof(*geom->idx),
           geom->idx, (size_t)ni * sizeof(*geom->idx));
    memset(&u, 0, sizeof(u));
    u.vp[0] = (float)width;
    u.vp[1] = (float)height;
    u.color[0] = rgba[0];
    u.color[1] = rgba[1];
    u.color[2] = rgba[2];
    u.color[3] = rgba[3];
    memcpy((unsigned char *)t->ubo[slot].mapped + uoff, &u, sizeof(u));
    t->vused[slot] = vbase + nv;
    t->iused[slot] = ibase + ni;
    t->ubo_used[slot] = di + 1;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, t->pipeline);
    vkCmdBindVertexBuffers(cmd, 0, 1, &t->vbo[slot].buf, &voff);
    vkCmdBindIndexBuffer(cmd, t->ibo[slot].buf, 0, VK_INDEX_TYPE_UINT16);
    ubo_info = (VkDescriptorBufferInfo){
        .buffer = t->ubo[slot].buf,
        .offset = uoff,
        .range = sizeof(u),
    };
    img_info = (VkDescriptorImageInfo){
        .sampler = font->sampler,
        .imageView = font->view,
        .imageLayout = font->layout,
    };
    writes[0] = (VkWriteDescriptorSet){
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .pBufferInfo = &ubo_info,
    };
    writes[1] = (VkWriteDescriptorSet){
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstBinding = 1,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &img_info,
    };
    vkCmdPushDescriptorSet(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, t->dev->push_layout, 0, 2, writes);
    vkCmdDrawIndexed(cmd, ni, 1, ibase, (int32_t)vbase, 0);
}

void wp_text_destroy(struct wp_text *t)
{
    uint32_t i;
    if (!t || !t->dev)
        return;
    vkDeviceWaitIdle(t->dev->device);
    if (t->pipeline)
        vkDestroyPipeline(t->dev->device, t->pipeline, NULL);
    if (t->vs)
        vkDestroyShaderModule(t->dev->device, t->vs, NULL);
    if (t->fs)
        vkDestroyShaderModule(t->dev->device, t->fs, NULL);
    for (i = 0; i < WP_SWAPCHAIN_IMAGES; i++) {
        wp_buffer_destroy(t->dev, &t->ubo[i]);
        wp_buffer_destroy(t->dev, &t->vbo[i]);
        wp_buffer_destroy(t->dev, &t->ibo[i]);
    }
    memset(t, 0, sizeof(*t));
}

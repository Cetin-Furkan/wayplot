#define _GNU_SOURCE
#include "renderer/card.h"

#include "renderer/camera.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

struct card_ubo {
    float vp[4];
    float color[4];
};

#define CARD_UBO_STRIDE 256

static const unsigned char card_vert_spv[] __attribute__((aligned(4))) = {
#embed "shaders/card.vert.spv"
};
static const unsigned char card_frag_spv[] __attribute__((aligned(4))) = {
#embed "shaders/card.frag.spv"
};

int wp_rect_ok(const struct wp_rect *r)
{
    return r && r->w > 0.0f && r->h > 0.0f;
}

struct wp_rect wp_rect_scaled(struct wp_rect r, float s)
{
    return (struct wp_rect){ r.x * s, r.y * s, r.w * s, r.h * s };
}

int wp_card_cpu(float x, float y, float w, float h, struct wp_card_geom *g)
{
    if (!g || w <= 0.0f || h <= 0.0f)
        return -EINVAL;
    memset(g, 0, sizeof(*g));
    /* TL, BL, BR, TR — same as text. Front under pixel ortho + CCW. */
    g->v[0] = (struct wp_card_vertex){ x, y };
    g->v[1] = (struct wp_card_vertex){ x, y + h };
    g->v[2] = (struct wp_card_vertex){ x + w, y + h };
    g->v[3] = (struct wp_card_vertex){ x + w, y };
    g->idx[0] = 0;
    g->idx[1] = 1;
    g->idx[2] = 2;
    g->idx[3] = 0;
    g->idx[4] = 2;
    g->idx[5] = 3;
    g->x0 = x;
    g->y0 = y;
    g->x1 = x + w;
    g->y1 = y + h;
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

static int make_pipeline(struct wp_card *c)
{
    VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = c->vs,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = c->fs,
            .pName = "main",
        },
    };
    VkVertexInputBindingDescription bind = {
        .binding = 0,
        .stride = sizeof(struct wp_card_vertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };
    VkVertexInputAttributeDescription attr[1] = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 0 },
    };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &bind,
        .vertexAttributeDescriptionCount = 1,
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
        .pColorAttachmentFormats = &c->color_fmt,
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
        .layout = c->dev->push_layout,
    };
    if (vkCreateGraphicsPipelines(c->dev->device, VK_NULL_HANDLE, 1, &pci, NULL, &c->pipeline) != VK_SUCCESS)
        return -EIO;
    return 0;
}

int wp_card_init(struct wp_card *c, struct wp_device *d, VkFormat color_fmt)
{
    uint32_t i;
    int ret;
    VkDeviceSize vbytes = (VkDeviceSize)WP_CARD_MAX_DRAWS * WP_CARD_VERTS * sizeof(struct wp_card_vertex);
    VkDeviceSize ibytes = (VkDeviceSize)WP_CARD_MAX_DRAWS * WP_CARD_INDS * sizeof(uint16_t);

    if (!c || !d)
        return -EINVAL;
    memset(c, 0, sizeof(*c));
    c->dev = d;
    c->color_fmt = color_fmt;
    for (i = 0; i < WP_SWAPCHAIN_IMAGES; i++) {
        ret = wp_buffer_create(d, (VkDeviceSize)WP_CARD_MAX_DRAWS * CARD_UBO_STRIDE,
                               VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               &c->ubo[i]);
        if (ret < 0)
            return ret;
        if (!c->ubo[i].mapped)
            return -ENOMEM;
        ret = wp_buffer_create(d, vbytes,
                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               &c->vbo[i]);
        if (ret < 0)
            return ret;
        if (!c->vbo[i].mapped)
            return -ENOMEM;
        ret = wp_buffer_create(d, ibytes,
                               VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               &c->ibo[i]);
        if (ret < 0)
            return ret;
        if (!c->ibo[i].mapped)
            return -ENOMEM;
        c->ubo_used[i] = 0;
        c->vused[i] = 0;
        c->iused[i] = 0;
    }
    c->vs = make_module(d->device, card_vert_spv, sizeof(card_vert_spv));
    c->fs = make_module(d->device, card_frag_spv, sizeof(card_frag_spv));
    if (!c->vs || !c->fs) {
        fprintf(stderr, "card: shader module failed vert=%zu frag=%zu\n",
                sizeof(card_vert_spv), sizeof(card_frag_spv));
        return -EIO;
    }
    ret = make_pipeline(c);
    if (ret < 0)
        fprintf(stderr, "card: pipeline failed\n");
    return ret;
}

void wp_card_reset(struct wp_card *c, uint32_t slot)
{
    if (!c || slot >= WP_SWAPCHAIN_IMAGES)
        return;
    c->ubo_used[slot] = 0;
    c->vused[slot] = 0;
    c->iused[slot] = 0;
}

void wp_card_draw(struct wp_card *c, VkCommandBuffer cmd, uint32_t width, uint32_t height,
                  uint32_t slot, const struct wp_card_geom *geom, const float rgba[4])
{
    struct card_ubo u;
    VkDeviceSize voff = 0;
    VkDeviceSize uoff;
    VkDescriptorBufferInfo ubo_info;
    VkWriteDescriptorSet writes[1];
    uint32_t vbase, ibase, di;
    uint32_t vmax = WP_CARD_MAX_DRAWS * WP_CARD_VERTS;
    uint32_t imax = WP_CARD_MAX_DRAWS * WP_CARD_INDS;

    if (!c || !cmd || !geom || !rgba || slot >= WP_SWAPCHAIN_IMAGES)
        return;
    if (width == 0 || height == 0)
        return;
    vbase = c->vused[slot];
    ibase = c->iused[slot];
    if (vbase > vmax || ibase > imax)
        return;
    if (WP_CARD_VERTS > vmax - vbase || WP_CARD_INDS > imax - ibase)
        return;
    if (c->ubo_used[slot] >= WP_CARD_MAX_DRAWS)
        return;
    di = c->ubo_used[slot];
    uoff = (VkDeviceSize)di * CARD_UBO_STRIDE;
    memcpy((unsigned char *)c->vbo[slot].mapped + (size_t)vbase * sizeof(geom->v[0]),
           geom->v, sizeof(geom->v));
    memcpy((unsigned char *)c->ibo[slot].mapped + (size_t)ibase * sizeof(geom->idx[0]),
           geom->idx, sizeof(geom->idx));
    memset(&u, 0, sizeof(u));
    u.vp[0] = (float)width;
    u.vp[1] = (float)height;
    u.color[0] = rgba[0];
    u.color[1] = rgba[1];
    u.color[2] = rgba[2];
    u.color[3] = rgba[3];
    memcpy((unsigned char *)c->ubo[slot].mapped + uoff, &u, sizeof(u));
    c->vused[slot] = vbase + WP_CARD_VERTS;
    c->iused[slot] = ibase + WP_CARD_INDS;
    c->ubo_used[slot] = di + 1;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c->pipeline);
    vkCmdBindVertexBuffers(cmd, 0, 1, &c->vbo[slot].buf, &voff);
    vkCmdBindIndexBuffer(cmd, c->ibo[slot].buf, 0, VK_INDEX_TYPE_UINT16);
    ubo_info = (VkDescriptorBufferInfo){
        .buffer = c->ubo[slot].buf,
        .offset = uoff,
        .range = sizeof(u),
    };
    writes[0] = (VkWriteDescriptorSet){
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .pBufferInfo = &ubo_info,
    };
    vkCmdPushDescriptorSet(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c->dev->push_layout, 0, 1, writes);
    vkCmdDrawIndexed(cmd, WP_CARD_INDS, 1, ibase, (int32_t)vbase, 0);
}

void wp_card_destroy(struct wp_card *c)
{
    uint32_t i;
    if (!c || !c->dev)
        return;
    vkDeviceWaitIdle(c->dev->device);
    if (c->pipeline)
        vkDestroyPipeline(c->dev->device, c->pipeline, NULL);
    if (c->vs)
        vkDestroyShaderModule(c->dev->device, c->vs, NULL);
    if (c->fs)
        vkDestroyShaderModule(c->dev->device, c->fs, NULL);
    for (i = 0; i < WP_SWAPCHAIN_IMAGES; i++) {
        wp_buffer_destroy(c->dev, &c->ubo[i]);
        wp_buffer_destroy(c->dev, &c->vbo[i]);
        wp_buffer_destroy(c->dev, &c->ibo[i]);
    }
    memset(c, 0, sizeof(*c));
}

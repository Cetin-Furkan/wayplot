#define _GNU_SOURCE
#include "renderer/lit.h"

#include "helper/math3d.h"

#include <errno.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>

struct lit_ubo {
    float mvp[16];
    float model[16];
    float light_dir[4];
};

/* Vulkan max minUniformBufferOffsetAlignment. */
#define LIT_UBO_STRIDE 256

static const unsigned char lit_vert_spv[] __attribute__((aligned(4))) = {
#embed "shaders/lit.vert.spv"
};
static const unsigned char lit_frag_spv[] __attribute__((aligned(4))) = {
#embed "shaders/lit.frag.spv"
};

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

static int make_pipeline(struct wp_lit *l)
{
    VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = l->vs,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = l->fs,
            .pName = "main",
        },
    };
    VkVertexInputBindingDescription bind = {
        .binding = 0,
        .stride = sizeof(struct wp_vn_vertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };
    VkVertexInputAttributeDescription attr[4] = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(struct wp_vn_vertex, px) },
        { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(struct wp_vn_vertex, nx) },
        { .location = 2, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(struct wp_vn_vertex, r) },
        { .location = 3, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(struct wp_vn_vertex, u) },
    };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &bind,
        .vertexAttributeDescriptionCount = 4,
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
        /* Owned by the camera. Not a per-mesh knob. Vulkan a = -shoelace. */
        .frontFace = wp_camera_front_clockwise() ? VK_FRONT_FACE_CLOCKWISE
                                                 : VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_LESS,
    };
    VkPipelineColorBlendAttachmentState ba = {
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
        .pColorAttachmentFormats = &l->color_fmt,
        .depthAttachmentFormat = VK_FORMAT_D32_SFLOAT,
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
        .pDepthStencilState = &ds,
        .pColorBlendState = &cb,
        .pDynamicState = &dyn,
        .layout = l->dev->push_layout,
    };
    if (vkCreateGraphicsPipelines(l->dev->device, VK_NULL_HANDLE, 1, &pci, NULL, &l->pipeline) != VK_SUCCESS)
        return -EIO;
    return 0;
}

static int make_albedo(struct wp_lit *l)
{
    const uint32_t px = 0xffffffffu;
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { 1, 1, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_HOST_TRANSFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkDeviceImageMemoryRequirements q = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS,
        .pCreateInfo = &ici,
    };
    VkMemoryRequirements2 memreq = { .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
    VkPhysicalDeviceMemoryProperties mprops;
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkHostImageLayoutTransitionInfo tr = {
        .sType = VK_STRUCTURE_TYPE_HOST_IMAGE_LAYOUT_TRANSITION_INFO,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    VkMemoryToImageCopy region = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_TO_IMAGE_COPY,
        .pHostPointer = &px,
        .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .imageExtent = { 1, 1, 1 },
    };
    VkCopyMemoryToImageInfo cpy = {
        .sType = VK_STRUCTURE_TYPE_COPY_MEMORY_TO_IMAGE_INFO,
        .dstImageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .regionCount = 1,
        .pRegions = &region,
    };
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = 1.0f,
    };
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    uint32_t idx;

    if (!l->dev->host_image_copy)
        return -ENOTSUP;
    vkGetDeviceImageMemoryRequirements(l->dev->device, &q, &memreq);
    if (vkCreateImage(l->dev->device, &ici, NULL, &l->albedo) != VK_SUCCESS) {
        ici.tiling = VK_IMAGE_TILING_LINEAR;
        q.pCreateInfo = &ici;
        vkGetDeviceImageMemoryRequirements(l->dev->device, &q, &memreq);
        if (vkCreateImage(l->dev->device, &ici, NULL, &l->albedo) != VK_SUCCESS)
            return -EIO;
    }
    vkGetPhysicalDeviceMemoryProperties(l->dev->phy, &mprops);
    idx = wp_find_memory_type(&mprops, memreq.memoryRequirements.memoryTypeBits,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (idx == UINT32_MAX)
        idx = wp_find_memory_type(&mprops, memreq.memoryRequirements.memoryTypeBits, 0);
    if (idx == UINT32_MAX)
        return -ENOMEM;
    ai.allocationSize = memreq.memoryRequirements.size;
    ai.memoryTypeIndex = idx;
    if (vkAllocateMemory(l->dev->device, &ai, NULL, &l->albedo_mem) != VK_SUCCESS)
        return -ENOMEM;
    if (vkBindImageMemory(l->dev->device, l->albedo, l->albedo_mem, 0) != VK_SUCCESS)
        return -EIO;
    tr.image = l->albedo;
    if (vkTransitionImageLayout(l->dev->device, 1, &tr) != VK_SUCCESS)
        return -EIO;
    cpy.dstImage = l->albedo;
    if (vkCopyMemoryToImage(l->dev->device, &cpy) != VK_SUCCESS)
        return -EIO;
    tr.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    tr.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (vkTransitionImageLayout(l->dev->device, 1, &tr) == VK_SUCCESS)
        l->albedo_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    else
        l->albedo_layout = VK_IMAGE_LAYOUT_GENERAL;
    vci.image = l->albedo;
    if (vkCreateImageView(l->dev->device, &vci, NULL, &l->albedo_view) != VK_SUCCESS)
        return -EIO;
    if (vkCreateSampler(l->dev->device, &sci, NULL, &l->sampler) != VK_SUCCESS)
        return -EIO;
    return 0;
}

int wp_lit_init(struct wp_lit *l, struct wp_device *d, VkFormat color_fmt)
{
    uint32_t i;
    int ret;

    if (!l || !d)
        return -EINVAL;
    memset(l, 0, sizeof(*l));
    l->dev = d;
    l->color_fmt = color_fmt;

    for (i = 0; i < WP_SWAPCHAIN_IMAGES; i++) {
        ret = wp_buffer_create(d, (VkDeviceSize)WP_LIT_MAX_DRAWS * LIT_UBO_STRIDE,
                               VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               &l->ubo[i]);
        if (ret < 0)
            return ret;
        if (!l->ubo[i].mapped)
            return -ENOMEM;
        l->ubo_used[i] = 0;
    }

    l->vs = make_module(d->device, lit_vert_spv, sizeof(lit_vert_spv));
    l->fs = make_module(d->device, lit_frag_spv, sizeof(lit_frag_spv));
    if (!l->vs || !l->fs) {
        fprintf(stderr, "lit: shader module failed vert=%zu frag=%zu\n",
                sizeof(lit_vert_spv), sizeof(lit_frag_spv));
        return -EIO;
    }
    ret = make_pipeline(l);
    if (ret < 0) {
        fprintf(stderr, "lit: pipeline failed\n");
        return ret;
    }
    ret = make_albedo(l);
    if (ret < 0) {
        fprintf(stderr, "lit: albedo failed (%s)\n", strerror(-ret));
        return ret;
    }
    return 0;
}

void wp_lit_reset(struct wp_lit *l, uint32_t slot)
{
    if (!l || slot >= WP_SWAPCHAIN_IMAGES)
        return;
    l->ubo_used[slot] = 0;
}

void wp_lit_draw(struct wp_lit *l, VkCommandBuffer cmd, uint32_t width, uint32_t height,
                 uint32_t slot, const struct wp_mesh *mesh, const struct wp_camera *cam,
                 const float model[16])
{
    wp_lit_draw_tex(l, cmd, width, height, slot, mesh, cam, model, NULL);
}

void wp_lit_draw_tex(struct wp_lit *l, VkCommandBuffer cmd, uint32_t width, uint32_t height,
                     uint32_t slot, const struct wp_mesh *mesh, const struct wp_camera *cam,
                     const float model[16], const struct wp_tex *tex)
{
    struct lit_ubo u;
    float view[16], proj[16], mv[16];
    VkDeviceSize voff = 0;
    VkDeviceSize uoff;
    uint32_t di;
    VkDescriptorBufferInfo ubo_info;
    VkDescriptorImageInfo img_info;
    VkWriteDescriptorSet writes[2];
    float aspect;

    if (!l || !cmd || !mesh || !cam || !model || slot >= WP_SWAPCHAIN_IMAGES)
        return;
    if (width == 0 || height == 0)
        return;
    if (l->ubo_used[slot] >= WP_LIT_MAX_DRAWS)
        return;
    di = l->ubo_used[slot];
    uoff = (VkDeviceSize)di * LIT_UBO_STRIDE;

    aspect = (float)width / (float)height;
    wp_camera_view(cam, view);
    wp_camera_proj(cam, aspect, proj);
    wp_mat4_mul(mv, view, model);
    wp_mat4_mul(u.mvp, proj, mv);
    /* Column-major float[16] == shader mvp_c0..c3 / model_c0..c3. No transpose. */
    memcpy(u.model, model, sizeof(u.model));
    u.light_dir[0] = 0.35f;
    u.light_dir[1] = -1.0f;
    u.light_dir[2] = -0.25f;
    u.light_dir[3] = 0.0f;
    memcpy((unsigned char *)l->ubo[slot].mapped + uoff, &u, sizeof(u));
    l->ubo_used[slot] = di + 1;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, l->pipeline);
    vkCmdBindVertexBuffers(cmd, 0, 1, &mesh->vbo.buf, &voff);
    vkCmdBindIndexBuffer(cmd, mesh->ibo.buf, 0, VK_INDEX_TYPE_UINT16);

    ubo_info = (VkDescriptorBufferInfo){
        .buffer = l->ubo[slot].buf,
        .offset = uoff,
        .range = sizeof(u),
    };
    if (tex && tex->view && tex->sampler) {
        img_info = (VkDescriptorImageInfo){
            .sampler = tex->sampler,
            .imageView = tex->view,
            .imageLayout = tex->layout,
        };
    } else {
        img_info = (VkDescriptorImageInfo){
            .sampler = l->sampler,
            .imageView = l->albedo_view,
            .imageLayout = l->albedo_layout,
        };
    }
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
    vkCmdPushDescriptorSet(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, l->dev->push_layout, 0, 2, writes);
    vkCmdDrawIndexed(cmd, mesh->index_count, 1, 0, 0, 0);
}

void wp_lit_destroy(struct wp_lit *l)
{
    uint32_t i;
    if (!l || !l->dev)
        return;
    vkDeviceWaitIdle(l->dev->device);
    if (l->sampler)
        vkDestroySampler(l->dev->device, l->sampler, NULL);
    if (l->albedo_view)
        vkDestroyImageView(l->dev->device, l->albedo_view, NULL);
    if (l->albedo)
        vkDestroyImage(l->dev->device, l->albedo, NULL);
    if (l->albedo_mem)
        vkFreeMemory(l->dev->device, l->albedo_mem, NULL);
    if (l->pipeline)
        vkDestroyPipeline(l->dev->device, l->pipeline, NULL);
    if (l->vs)
        vkDestroyShaderModule(l->dev->device, l->vs, NULL);
    if (l->fs)
        vkDestroyShaderModule(l->dev->device, l->fs, NULL);
    for (i = 0; i < WP_SWAPCHAIN_IMAGES; i++)
        wp_buffer_destroy(l->dev, &l->ubo[i]);
    memset(l, 0, sizeof(*l));
}

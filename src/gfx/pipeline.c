#include "khoros/gfx/pipeline.h"

#include <string.h>
#include <math.h>

alignas(uint32_t) static const uint8_t khr_card_vert_spv[] = {
#embed "shaders/card.vert.spv"
};

alignas(uint32_t) static const uint8_t khr_card_frag_spv[] = {
#embed "shaders/card.frag.spv"
};

alignas(uint32_t) static const uint8_t khr_plot_vert_spv[] = {
#embed "shaders/plot.vert.spv"
};

alignas(uint32_t) static const uint8_t khr_plot_frag_spv[] = {
#embed "shaders/plot.frag.spv"
};

khr_shader_bytecode_t khr_shader_get_card_vert(void) {
    return (khr_shader_bytecode_t){
        .code = (const uint32_t*)khr_card_vert_spv,
        .size_bytes = sizeof(khr_card_vert_spv),
    };
}

khr_shader_bytecode_t khr_shader_get_card_frag(void) {
    return (khr_shader_bytecode_t){
        .code = (const uint32_t*)khr_card_frag_spv,
        .size_bytes = sizeof(khr_card_frag_spv),
    };
}

khr_shader_bytecode_t khr_shader_get_plot_vert(void) {
    return (khr_shader_bytecode_t){
        .code = (const uint32_t*)khr_plot_vert_spv,
        .size_bytes = sizeof(khr_plot_vert_spv),
    };
}

khr_shader_bytecode_t khr_shader_get_plot_frag(void) {
    return (khr_shader_bytecode_t){
        .code = (const uint32_t*)khr_plot_frag_spv,
        .size_bytes = sizeof(khr_plot_frag_spv),
    };
}

bool khr_pipeline_layout_create(VkDevice dev,
                                VkDescriptorSetLayout set_layout,
                                uint32_t push_size,
                                VkPipelineLayout* out_layout) {
    if (!dev || !out_layout) return false;

    if (push_size == 0) {
        push_size = KHR_PUSH_CONSTANT_MAX_BYTES;
    }

    VkPushConstantRange pcr = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = push_size,
    };

    VkPipelineLayoutCreateInfo layout_ci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = (set_layout != VK_NULL_HANDLE) ? 1 : 0,
        .pSetLayouts = (set_layout != VK_NULL_HANDLE) ? &set_layout : nullptr,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcr,
    };

    return (vkCreatePipelineLayout(dev, &layout_ci, nullptr, out_layout) == VK_SUCCESS);
}

void khr_pipeline_layout_destroy(VkDevice dev, VkPipelineLayout layout) {
    if (dev && layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, layout, nullptr);
    }
}

bool khr_shader_module_create(VkDevice dev,
                              const uint32_t* spv_code,
                              size_t code_size,
                              VkShaderModule* out_module) {
    if (!dev || !spv_code || code_size < 4 || (code_size % 4) != 0 || !out_module) {
        return false;
    }
    VkShaderModuleCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = code_size,
        .pCode = spv_code,
    };
    return (vkCreateShaderModule(dev, &ci, nullptr, out_module) == VK_SUCCESS);
}

void khr_shader_module_destroy(VkDevice dev, VkShaderModule module) {
    if (dev && module != VK_NULL_HANDLE) {
        vkDestroyShaderModule(dev, module, nullptr);
    }
}

bool khr_gfx_pipeline_create(VkDevice dev,
                             const khr_gfx_pipeline_config_t* cfg,
                             VkPipeline* out_pipeline) {
    if (!dev || !cfg || !out_pipeline || cfg->layout == VK_NULL_HANDLE ||
        cfg->vs_module == VK_NULL_HANDLE) {
        return false;
    }
    *out_pipeline = VK_NULL_HANDLE;

    VkPipelineShaderStageCreateInfo stages[2];
    uint32_t stage_count = 0;

    stages[stage_count++] = (VkPipelineShaderStageCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_VERTEX_BIT,
        .module = cfg->vs_module,
        .pName = cfg->vs_entry ? cfg->vs_entry : "main",
    };

    if (cfg->fs_module != VK_NULL_HANDLE) {
        stages[stage_count++] = (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = cfg->fs_module,
            .pName = cfg->fs_entry ? cfg->fs_entry : "main",
        };
    }

    /* Zero vertex buffers: procedural vertex generation via SV_VertexID */
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 0,
        .pVertexBindingDescriptions = nullptr,
        .vertexAttributeDescriptionCount = 0,
        .pVertexAttributeDescriptions = nullptr,
    };

    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = (cfg->topology != 0) ? cfg->topology : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE,
    };

    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };

    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = cfg->cull_mode,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };

    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    VkPipelineColorBlendAttachmentState ba = {
        .blendEnable = cfg->blend_enable ? VK_TRUE : VK_FALSE,
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

    constexpr VkDynamicState dyn_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    VkPipelineDynamicStateCreateInfo dyn = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = sizeof(dyn_states) / sizeof(dyn_states[0]),
        .pDynamicStates = dyn_states,
    };

    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
    };

    VkPipelineRenderingCreateInfo rendering_ci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &cfg->color_format,
        .depthAttachmentFormat = cfg->depth_format,
        .stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
    };

    VkGraphicsPipelineCreateInfo gpci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering_ci,
        .stageCount = stage_count,
        .pStages = stages,
        .pVertexInputState = &vi,
        .pInputAssemblyState = &ia,
        .pViewportState = &vp,
        .pRasterizationState = &rs,
        .pMultisampleState = &ms,
        .pDepthStencilState = (cfg->depth_format != VK_FORMAT_UNDEFINED) ? &ds : nullptr,
        .pColorBlendState = &cb,
        .pDynamicState = &dyn,
        .layout = cfg->layout,
        .renderPass = VK_NULL_HANDLE, /* STRICTLY VK_NULL_HANDLE */
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = -1,
    };

    return (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, nullptr, out_pipeline) == VK_SUCCESS);
}

void khr_gfx_pipeline_destroy(VkDevice dev, VkPipeline pipeline) {
    if (dev && pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, pipeline, nullptr);
    }
}

bool khr_card_pipeline_init(khr_card_pipeline_t *p, const khr_gfx_device_t *d, VkFormat color_format) {
    if (!p || !d || d->device == VK_NULL_HANDLE) return false;
    memset(p, 0, sizeof(*p));
    p->dev = d;
    p->color_format = color_format;

    if (!khr_shader_module_create(d->device, (const uint32_t*)khr_card_vert_spv,
                                 sizeof(khr_card_vert_spv), &p->vs_module)) {
        return false;
    }

    if (!khr_shader_module_create(d->device, (const uint32_t*)khr_card_frag_spv,
                                 sizeof(khr_card_frag_spv), &p->fs_module)) {
        khr_card_pipeline_destroy(p);
        return false;
    }

    if (!khr_pipeline_layout_create(d->device, VK_NULL_HANDLE, sizeof(khr_card_push_t), &p->layout)) {
        khr_card_pipeline_destroy(p);
        return false;
    }

    khr_gfx_pipeline_config_t cfg = {
        .vs_module = p->vs_module,
        .vs_entry = "main",
        .fs_module = p->fs_module,
        .fs_entry = "main",
        .layout = p->layout,
        .color_format = color_format,
        .depth_format = VK_FORMAT_UNDEFINED,
        .blend_enable = true,
        .cull_mode = VK_CULL_MODE_NONE,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };

    if (!khr_gfx_pipeline_create(d->device, &cfg, &p->pipeline)) {
        khr_card_pipeline_destroy(p);
        return false;
    }

    return true;
}

void khr_card_pipeline_destroy(khr_card_pipeline_t *p) {
    if (!p || !p->dev) return;
    VkDevice dev = p->dev->device;
    if (p->pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, p->pipeline, nullptr);
        p->pipeline = VK_NULL_HANDLE;
    }
    if (p->layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, p->layout, nullptr);
        p->layout = VK_NULL_HANDLE;
    }
    if (p->fs_module != VK_NULL_HANDLE) {
        vkDestroyShaderModule(dev, p->fs_module, nullptr);
        p->fs_module = VK_NULL_HANDLE;
    }
    if (p->vs_module != VK_NULL_HANDLE) {
        vkDestroyShaderModule(dev, p->vs_module, nullptr);
        p->vs_module = VK_NULL_HANDLE;
    }
    memset(p, 0, sizeof(*p));
}

void khr_card_draw(const khr_card_pipeline_t *p, VkCommandBuffer cmd, const khr_card_push_t *push, uint32_t card_count) {
    if (!p || !p->pipeline || !cmd || !push || card_count == 0) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipeline);
    vkCmdPushConstants(cmd, p->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(khr_card_push_t), push);
    vkCmdDraw(cmd, 6, card_count, 0, 0);
}

bool khr_plot_pipeline_init(khr_plot_pipeline_t *p, const khr_gfx_device_t *d, VkFormat color_format) {
    if (!p || !d || d->device == VK_NULL_HANDLE) return false;
    memset(p, 0, sizeof(*p));
    p->dev = d;
    p->color_format = color_format;

    if (!khr_shader_module_create(d->device, (const uint32_t*)khr_plot_vert_spv,
                                 sizeof(khr_plot_vert_spv), &p->vs_module)) {
        return false;
    }

    if (!khr_shader_module_create(d->device, (const uint32_t*)khr_plot_frag_spv,
                                 sizeof(khr_plot_frag_spv), &p->fs_module)) {
        khr_plot_pipeline_destroy(p);
        return false;
    }

    if (!khr_pipeline_layout_create(d->device, VK_NULL_HANDLE, sizeof(khr_plot_push_t), &p->layout)) {
        khr_plot_pipeline_destroy(p);
        return false;
    }

    khr_gfx_pipeline_config_t cfg = {
        .vs_module = p->vs_module,
        .vs_entry = "main",
        .fs_module = p->fs_module,
        .fs_entry = "main",
        .layout = p->layout,
        .color_format = color_format,
        .depth_format = VK_FORMAT_UNDEFINED,
        .blend_enable = false,
        .cull_mode = VK_CULL_MODE_NONE,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };

    if (!khr_gfx_pipeline_create(d->device, &cfg, &p->pipeline)) {
        khr_plot_pipeline_destroy(p);
        return false;
    }

    return true;
}

void khr_plot_pipeline_destroy(khr_plot_pipeline_t *p) {
    if (!p || !p->dev) return;
    VkDevice dev = p->dev->device;
    if (p->pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, p->pipeline, nullptr);
        p->pipeline = VK_NULL_HANDLE;
    }
    if (p->layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, p->layout, nullptr);
        p->layout = VK_NULL_HANDLE;
    }
    if (p->fs_module != VK_NULL_HANDLE) {
        vkDestroyShaderModule(dev, p->fs_module, nullptr);
        p->fs_module = VK_NULL_HANDLE;
    }
    if (p->vs_module != VK_NULL_HANDLE) {
        vkDestroyShaderModule(dev, p->vs_module, nullptr);
        p->vs_module = VK_NULL_HANDLE;
    }
    memset(p, 0, sizeof(*p));
}

void khr_plot_draw(const khr_plot_pipeline_t *p, VkCommandBuffer cmd, const khr_plot_push_t *push, uint32_t segment_count) {
    if (!p || !p->pipeline || !cmd || !push || segment_count == 0) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipeline);
    vkCmdPushConstants(cmd, p->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(khr_plot_push_t), push);
    vkCmdDraw(cmd, 12, segment_count, 0, 0);
}

void khr_plot_fill_demo_samples(float* samples, uint32_t count) {
    if (samples == nullptr || count == 0) {
        return;
    }
    if (count == 1) {
        samples[0] = 0.5f;
        return;
    }
    const float two_pi = 6.283185307179586f;
    const float denom = (float)(count - 1U);
    for (uint32_t i = 0; i < count; i++) {
        float t = (float)i / denom;
        float s = 0.50f
                + 0.28f * sinf(t * two_pi * 2.0f)
                + 0.12f * sinf(t * two_pi * 5.0f)
                + 0.06f * sinf(t * two_pi * 11.0f);
        if (s < 0.05f) {
            s = 0.05f;
        }
        if (s > 0.95f) {
            s = 0.95f;
        }
        samples[i] = s;
    }
}

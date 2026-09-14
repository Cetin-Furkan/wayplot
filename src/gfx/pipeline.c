#include "khoros/gfx/pipeline.h"
#include "khoros/gfx/depth.h"
#include <math.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Embedded SPIR-V 1.6 Bytecode via C23 #embed
 * ------------------------------------------------------------------------- */
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
alignas(uint32_t) static const uint8_t khr_mesh_vert_spv[] = {
#embed "shaders/mesh.vert.spv"
};
alignas(uint32_t) static const uint8_t khr_mesh_frag_spv[] = {
#embed "shaders/mesh.frag.spv"
};

khr_shader_bytecode_t khr_shader_get_card_vert(void) {
    return (khr_shader_bytecode_t){ (const uint32_t*)(const void*)khr_card_vert_spv, sizeof(khr_card_vert_spv) };
}
khr_shader_bytecode_t khr_shader_get_card_frag(void) {
    return (khr_shader_bytecode_t){ (const uint32_t*)(const void*)khr_card_frag_spv, sizeof(khr_card_frag_spv) };
}
khr_shader_bytecode_t khr_shader_get_plot_vert(void) {
    return (khr_shader_bytecode_t){ (const uint32_t*)(const void*)khr_plot_vert_spv, sizeof(khr_plot_vert_spv) };
}
khr_shader_bytecode_t khr_shader_get_plot_frag(void) {
    return (khr_shader_bytecode_t){ (const uint32_t*)(const void*)khr_plot_frag_spv, sizeof(khr_plot_frag_spv) };
}
khr_shader_bytecode_t khr_shader_get_mesh_vert(void) {
    return (khr_shader_bytecode_t){ (const uint32_t*)(const void*)khr_mesh_vert_spv, sizeof(khr_mesh_vert_spv) };
}
khr_shader_bytecode_t khr_shader_get_mesh_frag(void) {
    return (khr_shader_bytecode_t){ (const uint32_t*)(const void*)khr_mesh_frag_spv, sizeof(khr_mesh_frag_spv) };
}

/* -------------------------------------------------------------------------
 * Pipeline Layout Creation & Destruction
 * ------------------------------------------------------------------------- */
bool khr_pipeline_layout_create(VkDevice dev,
                                VkDescriptorSetLayout set_layout,
                                uint32_t push_size,
                                VkPipelineLayout* out_layout) {
    if (dev == VK_NULL_HANDLE || out_layout == nullptr || push_size == 0) {
        return false;
    }

    VkPushConstantRange pcr = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = push_size,
    };

    VkPipelineLayoutCreateInfo layout_ci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = (set_layout != VK_NULL_HANDLE) ? 1 : 0,
        .pSetLayouts = (set_layout != VK_NULL_HANDLE) ? &set_layout : nullptr,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcr,
    };

    return (vkCreatePipelineLayout(dev, &layout_ci, nullptr, out_layout) == VK_SUCCESS);
}

void khr_pipeline_layout_destroy(VkDevice dev, VkPipelineLayout layout) {
    if (dev != VK_NULL_HANDLE && layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, layout, nullptr);
    }
}

/* -------------------------------------------------------------------------
 * Shader Module Creation & Destruction
 * ------------------------------------------------------------------------- */
bool khr_shader_module_create(VkDevice dev,
                              const uint32_t* spv_code,
                              size_t code_size,
                              VkShaderModule* out_module) {
    if (dev == VK_NULL_HANDLE || spv_code == nullptr || code_size < 4 || (code_size % 4) != 0 || out_module == nullptr) {
        return false;
    }

    VkShaderModuleCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .codeSize = code_size,
        .pCode = spv_code,
    };

    return (vkCreateShaderModule(dev, &ci, nullptr, out_module) == VK_SUCCESS);
}

void khr_shader_module_destroy(VkDevice dev, VkShaderModule module) {
    if (dev != VK_NULL_HANDLE && module != VK_NULL_HANDLE) {
        vkDestroyShaderModule(dev, module, nullptr);
    }
}

/* -------------------------------------------------------------------------
 * Core Dynamic Rendering Pipeline Factory
 * ------------------------------------------------------------------------- */
bool khr_gfx_pipeline_create(VkDevice dev,
                             const khr_gfx_pipeline_config_t* cfg,
                             VkPipeline* out_pipeline) {
    if (dev == VK_NULL_HANDLE || cfg == nullptr || out_pipeline == nullptr ||
        cfg->layout == VK_NULL_HANDLE || cfg->vs_module == VK_NULL_HANDLE) {
        return false;
    }
    *out_pipeline = VK_NULL_HANDLE;

    VkPipelineShaderStageCreateInfo stages[2];
    uint32_t stage_count = 0;

    stages[stage_count++] = (VkPipelineShaderStageCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stage = VK_SHADER_STAGE_VERTEX_BIT,
        .module = cfg->vs_module,
        .pName = cfg->vs_entry ? cfg->vs_entry : "main",
        .pSpecializationInfo = nullptr,
    };

    if (cfg->fs_module != VK_NULL_HANDLE) {
        stages[stage_count++] = (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = cfg->fs_module,
            .pName = cfg->fs_entry ? cfg->fs_entry : "main",
            .pSpecializationInfo = nullptr,
        };
    }

    /* Zero CPU vertex buffers: programmable vertex pulling via SV_VertexID or BDA pointers */
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .vertexBindingDescriptionCount = 0,
        .pVertexBindingDescriptions = nullptr,
        .vertexAttributeDescriptionCount = 0,
        .pVertexAttributeDescriptions = nullptr,
    };

    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .topology = (cfg->topology != 0) ? cfg->topology : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE,
    };

    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .viewportCount = 1,
        .pViewports = nullptr, /* Dynamic state */
        .scissorCount = 1,
        .pScissors = nullptr,   /* Dynamic state */
    };

    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = cfg->cull_mode,
        .frontFace = (cfg->front_face != 0) ? cfg->front_face : VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depthBiasEnable = VK_FALSE,
        .depthBiasConstantFactor = 0.0f,
        .depthBiasClamp = 0.0f,
        .depthBiasSlopeFactor = 0.0f,
        .lineWidth = 1.0f,
    };

    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .rasterizationSamples = (cfg->samples != 0) ? cfg->samples : VK_SAMPLE_COUNT_1_BIT,
        .sampleShadingEnable = VK_FALSE,
        .minSampleShading = 1.0f,
        .pSampleMask = nullptr,
        .alphaToCoverageEnable = VK_FALSE,
        .alphaToOneEnable = VK_FALSE,
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
        .pNext = nullptr,
        .flags = 0,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = 1,
        .pAttachments = &ba,
        .blendConstants = { 0.0f, 0.0f, 0.0f, 0.0f },
    };

    constexpr VkDynamicState dyn_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };

    VkPipelineDynamicStateCreateInfo dyn = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .dynamicStateCount = sizeof(dyn_states) / sizeof(dyn_states[0]),
        .pDynamicStates = dyn_states,
    };

    /* Depth-Stencil State:
     * When cfg->depth_test is true and a depth format exists: enable hardware Z-test and Z-write.
     * When cfg->depth_test is false (e.g. 2D UI overlay): disable testing and writing so cards do not occlude 3D geometry. */
    const bool has_depth = (cfg->depth_format != VK_FORMAT_UNDEFINED);

    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .depthTestEnable = (has_depth && cfg->depth_test) ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = (has_depth && cfg->depth_test) ? VK_TRUE : VK_FALSE,
        .depthCompareOp = (has_depth && cfg->depth_test) ? KHR_REVERSED_Z_COMPARE_OP : VK_COMPARE_OP_ALWAYS,
        .depthBoundsTestEnable = VK_FALSE,
        .stencilTestEnable = VK_FALSE,
        .front = {},
        .back = {},
        .minDepthBounds = 0.0f,
        .maxDepthBounds = 1.0f,
    };

    VkPipelineRenderingCreateInfo rendering_ci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .pNext = nullptr,
        .viewMask = 0,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &cfg->color_format,
        .depthAttachmentFormat = cfg->depth_format,
        .stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
    };

    VkGraphicsPipelineCreateInfo gpci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering_ci,
        .flags = 0,
        .stageCount = stage_count,
        .pStages = stages,
        .pVertexInputState = &vi,
        .pInputAssemblyState = &ia,
        .pTessellationState = nullptr,
        .pViewportState = &vp,
        .pRasterizationState = &rs,
        .pMultisampleState = &ms,
        .pDepthStencilState = has_depth ? &ds : nullptr,
        .pColorBlendState = &cb,
        .pDynamicState = &dyn,
        .layout = cfg->layout,
        .renderPass = VK_NULL_HANDLE, /* Pure Vulkan 1.4 Dynamic Rendering */
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = -1,
    };

    return (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, nullptr, out_pipeline) == VK_SUCCESS);
}

void khr_gfx_pipeline_destroy(VkDevice dev, VkPipeline pipeline) {
    if (dev != VK_NULL_HANDLE && pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, pipeline, nullptr);
    }
}

/* -------------------------------------------------------------------------
 * Pipeline 1: Card Pipeline (2D Analytical SDF UI Cards)
 * ------------------------------------------------------------------------- */
bool khr_card_pipeline_init(khr_card_pipeline_t *p, const khr_gfx_device_t *d, VkFormat color_format) {
    if (p == nullptr || d == nullptr || d->device == VK_NULL_HANDLE) {
        return false;
    }
    *p = (khr_card_pipeline_t){ .dev = d, .color_format = color_format };

    if (!khr_pipeline_layout_create(d->device, VK_NULL_HANDLE, sizeof(khr_card_push_t), &p->layout)) {
        return false;
    }

    auto vs = khr_shader_get_card_vert();
    auto fs = khr_shader_get_card_frag();
    if (!khr_shader_module_create(d->device, vs.code, vs.size_bytes, &p->vs_module) ||
        !khr_shader_module_create(d->device, fs.code, fs.size_bytes, &p->fs_module)) {
        khr_card_pipeline_destroy(p);
        return false;
    }

    khr_gfx_pipeline_config_t cfg = {
        .vs_module = p->vs_module,
        .fs_module = p->fs_module,
        .layout = p->layout,
        .color_format = color_format,
        .depth_format = khr_gfx_depth_format(d),
        .samples = khr_gfx_sample_count(d),
        .depth_test = false,                     /* 2D cards do not test or write depth */
        .blend_enable = true,                    /* Alpha blending for subpixel SDF edges */
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
    if (p == nullptr || p->dev == nullptr) {
        return;
    }
    VkDevice dev = p->dev->device;
    khr_gfx_pipeline_destroy(dev, p->pipeline);
    khr_shader_module_destroy(dev, p->fs_module);
    khr_shader_module_destroy(dev, p->vs_module);
    khr_pipeline_layout_destroy(dev, p->layout);
    *p = (khr_card_pipeline_t){};
}

void khr_card_draw(const khr_card_pipeline_t *p, VkCommandBuffer cmd, const khr_card_push_t *push, uint32_t card_count) {
    if (p == nullptr || cmd == VK_NULL_HANDLE || push == nullptr || card_count == 0) {
        return;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipeline);
    vkCmdPushConstants(cmd, p->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(*push), push);
    vkCmdDraw(cmd, 6, card_count, 0, 0); /* 6 procedural vertices per quad */
}

/* -------------------------------------------------------------------------
 * Pipeline 2: Plot Pipeline (Procedural 3D Ribbon)
 * ------------------------------------------------------------------------- */
bool khr_plot_pipeline_init(khr_plot_pipeline_t *p, const khr_gfx_device_t *d, VkFormat color_format) {
    if (p == nullptr || d == nullptr || d->device == VK_NULL_HANDLE) {
        return false;
    }
    *p = (khr_plot_pipeline_t){ .dev = d, .color_format = color_format };

    if (!khr_pipeline_layout_create(d->device, VK_NULL_HANDLE, sizeof(khr_plot_push_t), &p->layout)) {
        return false;
    }

    auto vs = khr_shader_get_plot_vert();
    auto fs = khr_shader_get_plot_frag();
    if (!khr_shader_module_create(d->device, vs.code, vs.size_bytes, &p->vs_module) ||
        !khr_shader_module_create(d->device, fs.code, fs.size_bytes, &p->fs_module)) {
        khr_plot_pipeline_destroy(p);
        return false;
    }

    khr_gfx_pipeline_config_t cfg = {
        .vs_module = p->vs_module,
        .fs_module = p->fs_module,
        .layout = p->layout,
        .color_format = color_format,
        .depth_format = khr_gfx_depth_format(d),
        .samples = khr_gfx_sample_count(d),
        .depth_test = true,                      /* 3D ribbon tests and writes depth */
        .blend_enable = false,
        .cull_mode = VK_CULL_MODE_NONE,          /* Ribbon has top & underside faces */
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };

    if (!khr_gfx_pipeline_create(d->device, &cfg, &p->pipeline)) {
        khr_plot_pipeline_destroy(p);
        return false;
    }
    return true;
}

void khr_plot_pipeline_destroy(khr_plot_pipeline_t *p) {
    if (p == nullptr || p->dev == nullptr) {
        return;
    }
    VkDevice dev = p->dev->device;
    khr_gfx_pipeline_destroy(dev, p->pipeline);
    khr_shader_module_destroy(dev, p->fs_module);
    khr_shader_module_destroy(dev, p->vs_module);
    khr_pipeline_layout_destroy(dev, p->layout);
    *p = (khr_plot_pipeline_t){};
}

void khr_plot_draw(const khr_plot_pipeline_t *p, VkCommandBuffer cmd, const khr_plot_push_t *push, uint32_t segment_count) {
    if (p == nullptr || cmd == VK_NULL_HANDLE || push == nullptr || segment_count == 0) {
        return;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipeline);
    vkCmdPushConstants(cmd, p->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(*push), push);
    vkCmdDraw(cmd, 12, segment_count, 0, 0); /* 12 procedural vertices per segment */
}

/* -------------------------------------------------------------------------
 * Pipeline 3: Mesh Pipeline (3D Geometry / Suzanne)
 * ------------------------------------------------------------------------- */
bool khr_mesh_pipeline_init(khr_mesh_pipeline_t *p, const khr_gfx_device_t *d, VkFormat color_format) {
    if (p == nullptr || d == nullptr || d->device == VK_NULL_HANDLE) {
        return false;
    }
    *p = (khr_mesh_pipeline_t){ .dev = d, .color_format = color_format };

    if (!khr_pipeline_layout_create(d->device, VK_NULL_HANDLE, sizeof(khr_mesh_push_t), &p->layout)) {
        return false;
    }

    auto vs = khr_shader_get_mesh_vert();
    auto fs = khr_shader_get_mesh_frag();
    if (!khr_shader_module_create(d->device, vs.code, vs.size_bytes, &p->vs_module) ||
        !khr_shader_module_create(d->device, fs.code, fs.size_bytes, &p->fs_module)) {
        khr_mesh_pipeline_destroy(p);
        return false;
    }

    khr_gfx_pipeline_config_t cfg = {
        .vs_module = p->vs_module,
        .fs_module = p->fs_module,
        .layout = p->layout,
        .color_format = color_format,
        .depth_format = khr_gfx_depth_format(d),
        .samples = khr_gfx_sample_count(d),
        .depth_test = true,                      /* 3D mesh tests and writes depth */
        .blend_enable = false,
        .cull_mode = VK_CULL_MODE_BACK_BIT,      /* Discard backfaces */
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };

    if (!khr_gfx_pipeline_create(d->device, &cfg, &p->pipeline)) {
        khr_mesh_pipeline_destroy(p);
        return false;
    }
    return true;
}

void khr_mesh_pipeline_destroy(khr_mesh_pipeline_t *p) {
    if (p == nullptr || p->dev == nullptr) {
        return;
    }
    VkDevice dev = p->dev->device;
    khr_gfx_pipeline_destroy(dev, p->pipeline);
    khr_shader_module_destroy(dev, p->fs_module);
    khr_shader_module_destroy(dev, p->vs_module);
    khr_pipeline_layout_destroy(dev, p->layout);
    *p = (khr_mesh_pipeline_t){};
}

void khr_mesh_draw(const khr_mesh_pipeline_t *p, VkCommandBuffer cmd, const khr_mesh_push_t *push) {
    if (p == nullptr || cmd == VK_NULL_HANDLE || push == nullptr || push->index_count == 0) {
        return;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipeline);
    vkCmdPushConstants(cmd, p->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(*push), push);
    vkCmdDraw(cmd, push->index_count, 1, 0, 0);
}

/* -------------------------------------------------------------------------
 * Host-Side Demo Sample Generator
 * ------------------------------------------------------------------------- */
void khr_plot_fill_demo_samples(float* samples, uint32_t count) {
    if (samples == nullptr || count == 0) {
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        float t = (float)i / (float)(count > 1 ? count - 1 : 1);
        samples[i] = 0.5f + 0.45f * sinf(t * 6.28318530717958647692f * 2.0f);
    }
}
#include "khoros/gfx/grid.h"
#include <string.h>
#include <stdio.h>

alignas(uint32_t) static const uint8_t khr_grid_vert_spv[] = {
#embed "shaders/grid.vert.spv"
};

alignas(uint32_t) static const uint8_t khr_grid_frag_spv[] = {
#embed "shaders/grid.frag.spv"
};

[[nodiscard]]
static VkShaderModule khr_grid_create_shader_module(VkDevice dev, const uint8_t* code, size_t size) {
    VkShaderModuleCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size,
        .pCode = (const uint32_t*)(const void*)code,
    };
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &ci, nullptr, &mod) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return mod;
}

[[nodiscard]]
bool khr_grid_pipeline_init(khr_grid_pipeline_t* p,
                            const khr_gfx_device_t* dev,
                            VkFormat color_fmt,
                            VkFormat depth_fmt) {
    if (p == nullptr || dev == nullptr || dev->device == VK_NULL_HANDLE) {
        return false;
    }
    memset(p, 0, sizeof(*p));
    p->dev = dev;

    VkDevice d = dev->device;

    /* 1. Create Push Constant Layout */
    VkPushConstantRange pcr = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(khr_grid_push_t),
    };

    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcr,
    };

    if (vkCreatePipelineLayout(d, &plci, nullptr, &p->layout) != VK_SUCCESS) {
        return false;
    }

    /* 2. Shader Stages */
    VkShaderModule vs_mod = khr_grid_create_shader_module(d, khr_grid_vert_spv, sizeof(khr_grid_vert_spv));
    VkShaderModule fs_mod = khr_grid_create_shader_module(d, khr_grid_frag_spv, sizeof(khr_grid_frag_spv));
    if (vs_mod == VK_NULL_HANDLE || fs_mod == VK_NULL_HANDLE) {
        if (vs_mod) vkDestroyShaderModule(d, vs_mod, nullptr);
        if (fs_mod) vkDestroyShaderModule(d, fs_mod, nullptr);
        vkDestroyPipelineLayout(d, p->layout, nullptr);
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vs_mod,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fs_mod,
            .pName = "main",
        }
    };

    /* 3. Pipeline Fixed Functions */
    VkPipelineVertexInputStateCreateInfo vici = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };

    VkPipelineInputAssemblyStateCreateInfo iaci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };

    VkDynamicState dyn_states[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dyn_states,
    };

    VkPipelineViewportStateCreateInfo vsci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };

    VkPipelineRasterizationStateCreateInfo rsci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .lineWidth = 1.0f,
    };

    VkPipelineMultisampleStateCreateInfo msci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = khr_gfx_sample_count(dev),
    };

    /* 4. Depth Stencil State (Reversed-Z: GEQUAL) */
    VkPipelineDepthStencilStateCreateInfo dsci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = (depth_fmt != VK_FORMAT_UNDEFINED) ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = (depth_fmt != VK_FORMAT_UNDEFINED) ? VK_TRUE : VK_FALSE,
        .depthCompareOp = (depth_fmt != VK_FORMAT_UNDEFINED) ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_ALWAYS,
    };

    /* 5. Alpha Blend State */
    VkPipelineColorBlendAttachmentState cba = {
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

    VkPipelineColorBlendStateCreateInfo cbsci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &cba,
    };

    /* 6. Dynamic Rendering Info */
    VkPipelineRenderingCreateInfo prci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_fmt,
        .depthAttachmentFormat = depth_fmt,
    };

    VkGraphicsPipelineCreateInfo gpci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &prci,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vici,
        .pInputAssemblyState = &iaci,
        .pViewportState = &vsci,
        .pRasterizationState = &rsci,
        .pMultisampleState = &msci,
        .pDepthStencilState = &dsci,
        .pColorBlendState = &cbsci,
        .pDynamicState = &dynamic_state,
        .layout = p->layout,
    };

    VkResult res = vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &gpci, nullptr, &p->pipeline);
    vkDestroyShaderModule(d, vs_mod, nullptr);
    vkDestroyShaderModule(d, fs_mod, nullptr);

    if (res != VK_SUCCESS) {
        vkDestroyPipelineLayout(d, p->layout, nullptr);
        p->layout = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

void khr_grid_pipeline_destroy(khr_grid_pipeline_t* p) {
    if (p == nullptr || p->dev == nullptr || p->dev->device == VK_NULL_HANDLE) {
        return;
    }
    VkDevice d = p->dev->device;
    if (p->pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(d, p->pipeline, nullptr);
        p->pipeline = VK_NULL_HANDLE;
    }
    if (p->layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(d, p->layout, nullptr);
        p->layout = VK_NULL_HANDLE;
    }
}

void khr_grid_pipeline_draw(const khr_grid_pipeline_t* p,
                            VkCommandBuffer cmd,
                            const khr_grid_push_t* push) {
    if (p == nullptr || cmd == VK_NULL_HANDLE || p->pipeline == VK_NULL_HANDLE || push == nullptr) {
        return;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipeline);
    vkCmdPushConstants(cmd, p->layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(*push), push);
    vkCmdDraw(cmd, 3, 1, 0, 0); /* 3 vertices generate full-screen unprojected triangle */
}

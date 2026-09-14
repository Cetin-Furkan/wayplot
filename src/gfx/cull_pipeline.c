#include "khoros/gfx/cull_pipeline.h"
#include "khoros/gfx/pipeline.h"
#include "khoros/gfx/depth.h"
#include <string.h>

/* Embedded SPIR-V Bytecode via C23 #embed */
alignas(uint32_t) static const uint8_t khr_cull_comp_spv[] = {
#embed "shaders/cull.comp.spv"
};

alignas(uint32_t) static const uint8_t khr_mesh_instanced_vert_spv[] = {
#embed "shaders/mesh_instanced.vert.spv"
};

alignas(uint32_t) static const uint8_t khr_mesh_instanced_frag_spv[] = {
#embed "shaders/mesh_instanced.frag.spv"
};

[[nodiscard]]
static bool khr_compute_pipeline_layout_create(VkDevice dev,
                                               uint32_t push_size,
                                               VkPipelineLayout* out_layout) {
    if (dev == VK_NULL_HANDLE || out_layout == nullptr || push_size == 0) {
        return false;
    }
    VkPushConstantRange pcr = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = push_size,
    };
    VkPipelineLayoutCreateInfo layout_ci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 0,
        .pSetLayouts = nullptr,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcr,
    };
    return (vkCreatePipelineLayout(dev, &layout_ci, nullptr, out_layout) == VK_SUCCESS);
}

[[nodiscard]]
bool khr_cull_pipeline_init(khr_cull_pipeline_t* p, const khr_gfx_device_t* d) {
    if (p == nullptr || d == nullptr || d->device == VK_NULL_HANDLE) {
        return false;
    }
    *p = (khr_cull_pipeline_t){ .dev = d };

    if (!khr_compute_pipeline_layout_create(d->device, sizeof(khr_cull_push_t), &p->layout)) {
        return false;
    }

    if (!khr_shader_module_create(d->device, (const uint32_t*)(const void*)khr_cull_comp_spv,
                                  sizeof(khr_cull_comp_spv), &p->comp_module)) {
        khr_pipeline_layout_destroy(d->device, p->layout);
        *p = (khr_cull_pipeline_t){};
        return false;
    }

    VkComputePipelineCreateInfo cp_ci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = p->comp_module,
            .pName = "main",
            .pSpecializationInfo = nullptr,
        },
        .layout = p->layout,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = -1,
    };

    if (vkCreateComputePipelines(d->device, VK_NULL_HANDLE, 1, &cp_ci, nullptr, &p->pipeline) != VK_SUCCESS) {
        khr_shader_module_destroy(d->device, p->comp_module);
        khr_pipeline_layout_destroy(d->device, p->layout);
        *p = (khr_cull_pipeline_t){};
        return false;
    }

    return true;
}

void khr_cull_pipeline_destroy(khr_cull_pipeline_t* p) {
    if (p == nullptr || p->dev == nullptr || p->dev->device == VK_NULL_HANDLE) {
        return;
    }
    VkDevice dev = p->dev->device;
    if (p->pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, p->pipeline, nullptr);
    }
    khr_shader_module_destroy(dev, p->comp_module);
    khr_pipeline_layout_destroy(dev, p->layout);
    *p = (khr_cull_pipeline_t){};
}

void khr_cull_pipeline_dispatch(const khr_cull_pipeline_t* p,
                                VkCommandBuffer cmd,
                                const khr_cull_push_t* push) {
    if (p == nullptr || cmd == VK_NULL_HANDLE || push == nullptr || push->instance_count == 0) {
        return;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipeline);
    vkCmdPushConstants(cmd, p->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(*push), push);

    uint32_t group_count = (push->instance_count + 63U) / 64U;
    vkCmdDispatch(cmd, group_count, 1, 1);
}

[[nodiscard]]
bool khr_mesh_instanced_pipeline_init(khr_mesh_instanced_pipeline_t* p,
                                      const khr_gfx_device_t* d,
                                      VkFormat color_format,
                                      const khr_descriptor_heap_t* heap) {
    if (p == nullptr || d == nullptr || d->device == VK_NULL_HANDLE) {
        return false;
    }
    *p = (khr_mesh_instanced_pipeline_t){ .dev = d, .color_format = color_format };

    if (heap != nullptr) {
        p->resource_layout = heap->resource_set_layout;
        p->sampler_layout = heap->sampler_set_layout;
        p->owns_layouts = false;
    } else {
        VkDescriptorSetLayoutBinding res_b = {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            .descriptorCount = 1024,
            .stageFlags = VK_SHADER_STAGE_ALL,
        };
        VkDescriptorSetLayoutCreateInfo res_ci = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
            .bindingCount = 1,
            .pBindings = &res_b,
        };
        if (vkCreateDescriptorSetLayout(d->device, &res_ci, nullptr, &p->resource_layout) != VK_SUCCESS) {
            return false;
        }

        VkDescriptorSetLayoutBinding samp_b = {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
            .descriptorCount = 64,
            .stageFlags = VK_SHADER_STAGE_ALL,
        };
        VkDescriptorSetLayoutCreateInfo samp_ci = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
            .bindingCount = 1,
            .pBindings = &samp_b,
        };
        if (vkCreateDescriptorSetLayout(d->device, &samp_ci, nullptr, &p->sampler_layout) != VK_SUCCESS) {
            vkDestroyDescriptorSetLayout(d->device, p->resource_layout, nullptr);
            return false;
        }
        p->owns_layouts = true;
    }

    VkDescriptorSetLayout set_layouts[2] = { p->resource_layout, p->sampler_layout };
    VkPushConstantRange pcr = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(khr_mesh_instanced_push_t),
    };
    VkPipelineLayoutCreateInfo layout_ci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 2,
        .pSetLayouts = set_layouts,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcr,
    };
    if (vkCreatePipelineLayout(d->device, &layout_ci, nullptr, &p->layout) != VK_SUCCESS) {
        if (p->owns_layouts) {
            vkDestroyDescriptorSetLayout(d->device, p->sampler_layout, nullptr);
            vkDestroyDescriptorSetLayout(d->device, p->resource_layout, nullptr);
        }
        return false;
    }

    if (!khr_shader_module_create(d->device, (const uint32_t*)(const void*)khr_mesh_instanced_vert_spv,
                                  sizeof(khr_mesh_instanced_vert_spv), &p->vs_module) ||
        !khr_shader_module_create(d->device, (const uint32_t*)(const void*)khr_mesh_instanced_frag_spv,
                                  sizeof(khr_mesh_instanced_frag_spv), &p->fs_module)) {
        khr_mesh_instanced_pipeline_destroy(p);
        return false;
    }

    khr_gfx_pipeline_config_t cfg = {
        .vs_module = p->vs_module,
        .fs_module = p->fs_module,
        .layout = p->layout,
        .color_format = color_format,
        .depth_format = khr_gfx_depth_format(d),
        .samples = khr_gfx_sample_count(d),
        .depth_test = true,
        .blend_enable = false,
        .cull_mode = VK_CULL_MODE_NONE,
        .front_face = VK_FRONT_FACE_CLOCKWISE,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };

    if (!khr_gfx_pipeline_create(d->device, &cfg, &p->pipeline)) {
        khr_mesh_instanced_pipeline_destroy(p);
        return false;
    }
    return true;
}

void khr_mesh_instanced_pipeline_destroy(khr_mesh_instanced_pipeline_t* p) {
    if (p == nullptr || p->dev == nullptr || p->dev->device == VK_NULL_HANDLE) {
        return;
    }
    VkDevice dev = p->dev->device;
    if (p->pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, p->pipeline, nullptr);
    }
    khr_shader_module_destroy(dev, p->fs_module);
    khr_shader_module_destroy(dev, p->vs_module);
    khr_pipeline_layout_destroy(dev, p->layout);
    if (p->owns_layouts) {
        if (p->sampler_layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(dev, p->sampler_layout, nullptr);
        }
        if (p->resource_layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(dev, p->resource_layout, nullptr);
        }
    }
    *p = (khr_mesh_instanced_pipeline_t){};
}

void khr_mesh_instanced_draw_indirect(const khr_mesh_instanced_pipeline_t* p,
                                      VkCommandBuffer cmd,
                                      const khr_mesh_instanced_push_t* push,
                                      VkBuffer indirect_buffer,
                                      VkDeviceSize indirect_offset,
                                      uint32_t draw_count,
                                      uint32_t stride) {
    if (p == nullptr || cmd == VK_NULL_HANDLE || push == nullptr ||
        indirect_buffer == VK_NULL_HANDLE || draw_count == 0) {
        return;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipeline);
    vkCmdPushConstants(cmd, p->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(*push), push);
    vkCmdDrawIndirect(cmd, indirect_buffer, indirect_offset, draw_count, stride);
}

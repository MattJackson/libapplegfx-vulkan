/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "pipeline_build.h"
#include "common/log.h"

#ifdef LAGFX_HAVE_VULKAN

lagfx_status_t lagfx_pipeline_build(VkDevice device,
                                    const lagfx_pipeline_desc_t *desc,
                                    VkPipeline *out_pipeline) {
    if (!device || !desc || !desc->vertex_shader || !desc->fragment_shader
        || !out_pipeline) {
        return LAGFX_ERR_INVALID_ARG;
    }
    if (desc->layout == VK_NULL_HANDLE) {
        /* Vulkan spec (VUID-VkGraphicsPipelineCreateInfo-layout-06602):
         * layout must be valid even when shaders declare zero descriptor
         * bindings. Callers should pass the device's empty_layout for
         * the substitute triangle pipeline. */
        return LAGFX_ERR_INVALID_ARG;
    }

    /* Vertex input — empty struct as in triangle-lavapipe-e2e.c */
    VkPipelineVertexInputStateCreateInfo vin = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };

    /* Input assembly */
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };

    /* Viewport + scissor — hardcoded to match triangle test */
    VkViewport vp = {
        .x = 0.0f, .y = 0.0f, .width = 1920.0f, .height = 1080.0f,
        .minDepth = 0.0f, .maxDepth = 1.0f,
    };
    VkRect2D sc = { .offset = {0, 0}, .extent = { .width = 1920, .height = 1080 } };
    VkPipelineViewportStateCreateInfo vpst = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &vp,
        .scissorCount = 1, .pScissors = &sc,
    };

    /* Rasterization state — hardcoded to triangle test values */
    VkPipelineRasterizationStateCreateInfo rsst = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };

    /* Multisample state */
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    /* Color blend attachment — full write mask, no blending */
    VkPipelineColorBlendAttachmentState blend_att = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT
                        | VK_COLOR_COMPONENT_G_BIT
                        | VK_COLOR_COMPONENT_B_BIT
                        | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blend_att,
    };

    /* Shader stages — entry-point names default to "main" but can be
     * overridden via desc. The Stage 65d Option 3 substitute triangle
     * SPVs use "triangle_vertex" / "triangle_fragment" because they're
     * produced by the AIR-to-SPIRV translator, not glslang. */
    const char *v_entry = desc->vertex_entry_point   ? desc->vertex_entry_point   : "main";
    const char *f_entry = desc->fragment_entry_point ? desc->fragment_entry_point : "main";
    VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = desc->vertex_shader,
            .pName = v_entry,
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = desc->fragment_shader,
            .pName = f_entry,
        },
    };

    /* Dynamic rendering pNext chain (VK_KHR_dynamic_rendering) */
    const VkFormat color_fmt = desc->color_format != VK_FORMAT_UNDEFINED
                               ? desc->color_format
                               : VK_FORMAT_B8G8R8A8_UNORM;
    VkPipelineRenderingCreateInfo rendering_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_fmt,
    };

    /* Depth/stencil — NULL as in triangle test (no depth) */
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
    };

    /* Graphics pipeline create info */
    VkGraphicsPipelineCreateInfo gpci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering_info,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vin,
        .pInputAssemblyState = &ia,
        .pViewportState = &vpst,
        .pRasterizationState = &rsst,
        .pMultisampleState = &ms,
        .pDepthStencilState = &ds,
        .pColorBlendState = &cb,
        .layout = desc->layout,
        .renderPass = VK_NULL_HANDLE,
        .subpass = 0,
    };

    VkPipeline pipe = VK_NULL_HANDLE;
    VkResult vr = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci,
                                            NULL, &pipe);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("lagfx_pipeline_build: vkCreateGraphicsPipelines failed (%d)",
                  (int)vr);
        return LAGFX_ERR_BACKEND;
    }

    *out_pipeline = pipe;
    return LAGFX_OK;
}

#else  /* !LAGFX_HAVE_VULKAN */

/* Vulkan-disabled stub build: NULL out_pipeline + return error. */
lagfx_status_t lagfx_pipeline_build(VkDevice device,
                                    const lagfx_pipeline_desc_t *desc,
                                    VkPipeline *out_pipeline) {
    (void)device; (void)desc;
    if (out_pipeline) *out_pipeline = NULL;
    return LAGFX_ERR_NOT_FOUND;
}

#endif  /* LAGFX_HAVE_VULKAN */

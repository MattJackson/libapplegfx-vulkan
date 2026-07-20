/* SPDX-License-Identifier: MIT */
#include "pipeline_build.h"
#include "common/log.h"
#include "common/policy.h"

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

    /* Vertex input. Empty by default (procedural/vertex_id shaders). When the
     * reflected vertex shader declares stage-in attributes, build a non-empty
     * state (binding 0, tightly-packed R32..A32_SFLOAT) so the shader's
     * positions come from the bound guest vertex buffer instead of unbound
     * (= 0 = degenerate = black). Gated by the caller setting n_vtx_inputs. */
    VkVertexInputBindingDescription   vbind = {0};
    VkVertexInputAttributeDescription vattr[8];
    VkPipelineVertexInputStateCreateInfo vin = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    if (desc->n_vtx_inputs > 0u) {
        uint32_t nvi = desc->n_vtx_inputs > 8u ? 8u : desc->n_vtx_inputs;
        uint32_t off = 0u;
        for (uint32_t a = 0; a < nvi; a++) {
            uint32_t c = desc->vtx_in_comp[a]; if (c < 1u) c = 1u; if (c > 4u) c = 4u;
            VkFormat fmt = (c == 1u) ? VK_FORMAT_R32_SFLOAT :
                           (c == 2u) ? VK_FORMAT_R32G32_SFLOAT :
                           (c == 3u) ? VK_FORMAT_R32G32B32_SFLOAT :
                                       VK_FORMAT_R32G32B32A32_SFLOAT;
            vattr[a] = (VkVertexInputAttributeDescription){
                .location = desc->vtx_in_loc[a], .binding = 0u, .format = fmt, .offset = off,
            };
            off += c * 4u;
        }
        /* Vertex stride, in priority order:
         *  1. desc->vtx_stride — the REAL stride decoded from the PSO's serialized
         *     MTLVertexDescriptor (lagfx_parse_pso_vertex_stride). Authoritative
         *     and per-pipeline (CoreAnimation composites = 48, fullscreen quad = 24).
         *  2. LAGFX_VTX_STRIDE env override (diagnostic / global probe).
         *  3. round8(sum of attr sizes) — the legacy heuristic; correct only when
         *     there is no trailing padding, and WRONG for the CA composites (tight
         *     sum rounds to 24 while the real stride is 48 → every other vertex
         *     misread → horizontal-band smear). Fallback of last resort. */
        uint32_t stride = (off + 7u) & ~7u;
        if (desc->vtx_stride >= off && desc->vtx_stride <= 256u)
            stride = desc->vtx_stride;
        const char *vs = getenv("LAGFX_VTX_STRIDE");
        if (vs) { unsigned long s = strtoul(vs, NULL, 0); if (s >= off && s <= 256u) stride = (uint32_t)s; }
        vbind = (VkVertexInputBindingDescription){
            .binding = 0u, .stride = stride, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
        };
        vin.vertexBindingDescriptionCount   = 1u;
        vin.pVertexBindingDescriptions      = &vbind;
        vin.vertexAttributeDescriptionCount = nvi;
        vin.pVertexAttributeDescriptions    = vattr;
    }

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

    /* Color blend attachment — src-over alpha (M2q dumb-faithful): SkyLight
     * composites layers src-over; with loadOp=LOAD this makes a draw's
     * transparent/alpha-0 pixels PRESERVE what is already in the target
     * instead of replacing it with black (the "opaque clear wipes the
     * wallpaper" bug). Opaque (alpha=1) pixels — incl. the substitute
     * triangle — replace, exactly as before.
     * Kill-switch: LAGFX_DISABLE_M2_SRCOVER → no blending. */
    VkPipelineColorBlendAttachmentState blend_att = {
        .blendEnable         = LAGFX_POLICY("M2_SRCOVER") ? VK_TRUE : VK_FALSE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp        = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp        = VK_BLEND_OP_ADD,
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

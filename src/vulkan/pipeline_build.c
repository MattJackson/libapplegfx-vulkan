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
        /* GOAL-M2z: PSO-decoded attribute formats+offsets are authoritative
         * when present for every reflected attr (matched by order — both are
         * offset/location ordered). MTLVertexFormat -> VkFormat for the
         * observed set; unknown formats fall back to the reflected SFLOAT
         * tight-pack (and log, gated). The killer this fixes: fmt 9
         * (UChar4Normalized rgba8 color @32) was bound R32G32B32A32_SFLOAT ->
         * NaN color/alpha -> invisible panel fills. */
        bool use_pso_attrs = desc->n_pso_attrs >= nvi;
        for (uint32_t a = 0; use_pso_attrs && a < nvi; a++) {
            switch (desc->pso_attr_fmt[a]) {
            case 3: case 9: case 28: case 29: case 30: case 31:
            case 37: case 52: break;
            default: use_pso_attrs = false; break;
            }
        }
        for (uint32_t a = 0; a < nvi; a++) {
            uint32_t c = desc->vtx_in_comp[a]; if (c < 1u) c = 1u; if (c > 4u) c = 4u;
            VkFormat fmt = (c == 1u) ? VK_FORMAT_R32_SFLOAT :
                           (c == 2u) ? VK_FORMAT_R32G32_SFLOAT :
                           (c == 3u) ? VK_FORMAT_R32G32B32_SFLOAT :
                                       VK_FORMAT_R32G32B32A32_SFLOAT;
            uint32_t aoff = off;
            if (use_pso_attrs) {
                aoff = desc->pso_attr_off[a];
                switch (desc->pso_attr_fmt[a]) {
                case 3:  fmt = VK_FORMAT_R8G8B8A8_UINT;          break; /* UChar4 */
                case 9:  fmt = VK_FORMAT_B8G8R8A8_UNORM;         break; /* UChar4Normalized — CA pool colors are BGRA-ordered (the PBGRA pipes); R8G8B8A8 rendered blue UI as red */
                case 28: fmt = VK_FORMAT_R32_SFLOAT;             break; /* Float */
                case 29: fmt = VK_FORMAT_R32G32_SFLOAT;          break; /* Float2 */
                case 30: fmt = VK_FORMAT_R32G32B32_SFLOAT;       break; /* Float3 */
                case 31: fmt = VK_FORMAT_R32G32B32A32_SFLOAT;    break; /* Float4 */
                case 37: fmt = VK_FORMAT_R32_UINT;               break; /* UInt */
                case 52: fmt = VK_FORMAT_B8G8R8A8_UNORM;         break; /* UChar4Normalized_BGRA */
                }
            }
            vattr[a] = (VkVertexInputAttributeDescription){
                .location = desc->vtx_in_loc[a], .binding = 0u, .format = fmt, .offset = aoff,
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
        /* The decoded PSO stride is AUTHORITATIVE whenever present and sane.
         * lldb ground truth (GOAL-M2x, 2026-07-21, WindowServer
         * PGSerializerRenderCommandEncoder): the indexed-quad composite draws
         * (0x23/0x28/0x2c) bind a 48-byte-stride pool buffer — float4
         * SCREEN-SPACE pos@0 (w=1), float2 normalized texcoord@16, rgba8
         * color@32 — while the shader reflects only 2 attrs (extent 32). The
         * former "+8 B of the reflected extent" clamp rejected the real 48 →
         * stride 32 → v0 correct, v1/v2 read from attr padding → the
         * degenerate (v1==v2) triangles. The reflected extent is the shader's
         * VIEW of the vertex, not the buffer stride; only the PSO's serialized
         * MTLVertexDescriptor knows the buffer layout. */
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
        if (getenv("LAGFX_DUMP_SPV")) {
            char ab[128]; size_t an = 0;
            for (uint32_t a = 0; a < nvi && an + 24 < sizeof(ab); a++)
                an += (size_t)snprintf(ab+an, sizeof(ab)-an, "loc%u:c%u@%u ",
                                       vattr[a].location, desc->vtx_in_comp[a], vattr[a].offset);
            LAGFX_LOG("VTXINPUT n=%u stride=%u attrs: %s", nvi, stride, ab);
        }
    }

    /* Input assembly */
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };

    /* Viewport with the Metal→Vulkan Y-FLIP (negative height, origin at
     * y=height). Metal clip space is Y-UP; Vulkan is Y-DOWN — without the flip
     * the guest's composite geometry renders vertically MIRRORED (proven live:
     * the cursor drew huge + UPSIDE-DOWN and UI rows collapsed into horizontal
     * bands, while our upright display test-boxes confirmed the display path
     * itself is correct). A negative-height viewport (Vulkan 1.1 / KHR_
     * maintenance1) is the canonical fix. Kill-switch: LAGFX_DISABLE_YFLIP. */
    bool yflip = (getenv("LAGFX_DISABLE_YFLIP") == NULL);
    VkViewport vp = {
        .x = 0.0f, .y = yflip ? 1080.0f : 0.0f,
        .width = 1920.0f, .height = yflip ? -1080.0f : 1080.0f,
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

    /* DYNAMIC viewport + scissor: the same pipeline draws into targets of
     * DIFFERENT sizes (the 1920×1080 scanout AND 1280×1024 per-pass surfaces).
     * A pipeline-baked 1920×1080 viewport mis-scaled the 1280×1024 per-pass
     * draws (the "zoom"). The draw site sets the viewport per-draw to the ACTUAL
     * render-target dims (with the Y-flip). Static vp/sc above become ignored
     * placeholders. */
    VkDynamicState dyn_states[2] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
    };
    VkPipelineDynamicStateCreateInfo dyn = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn_states,
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
        .pDynamicState = &dyn,
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

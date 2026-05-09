/*
 * libapplegfx-vulkan — Metal-to-Vulkan render command encoder (Phase 3.A)
 * src/translate/render_encoder.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implements the Phase 3.A skeleton. See render_encoder.h for the
 * overall design; this file is the straight-line impl.
 *
 * Choice notes:
 *
 *  - Dynamic rendering (VK_KHR_dynamic_rendering, core in Vulkan
 *    1.3) is the one render-pass path. No VkRenderPass/VkFramebuffer
 *    fallback — Phase 2.B already proved dynamic rendering on
 *    lavapipe, and rev-9 of the metal-implementation-plan requires
 *    it end-to-end. If vk->have_dynamic_rendering is false we bail
 *    rather than spin up a render-pass-object path.
 *
 *  - Push descriptors (vkCmdPushDescriptorSetKHR) are used for
 *    texture bindings. Matches the "no per-draw descriptor pool
 *    churn" policy in paravirt-re/phase-3-metal-vulkan-plan.md §3.A
 *    and keeps the encoder stateless across draws. Requires
 *    VK_KHR_push_descriptor; the encoder does NOT probe for it
 *    here because that's an instance/device init concern — it
 *    logs-and-bails if the extension isn't wired.
 *
 *  - Pipeline binding at this phase is a RECORD-ONLY operation.
 *    We store the requested shader_kind + VkPipelineLayout; the
 *    actual vkCmdBindPipeline happens when Phase 3.E creates
 *    VkShader/VkPipeline objects from the shader catalog. Tests
 *    exercise the state-machine + argument-validation paths now;
 *    the Vulkan-level draw becomes real once Phase 3.E lands.
 */

#include "render_encoder.h"
#include "common/log.h"

#ifdef LAGFX_HAVE_VULKAN
#  include "vulkan/instance.h"
#  include "vulkan/pipeline.h"
#endif

#include <stdlib.h>
#include <string.h>

#ifdef LAGFX_HAVE_VULKAN

/* --- Push-descriptor dispatch cache --------------------------
 *
 * vkCmdPushDescriptorSetKHR is an instance-level function pointer
 * that must be resolved via vkGetDeviceProcAddr because it comes
 * from VK_KHR_push_descriptor (promoted to core in Vulkan 1.4, but
 * still gated on a valid load). Cache lives on lagfx_vk_state
 * (vk->push_desc_pfn) so each VkDevice carries its own pfn — fixes
 * the single-VkDevice limitation that the previous file-scope static
 * imposed. */

/* Phase 3.A scaffold doesn't yet plumb the VkDevice down here (the
 * encoder state carries the command buffer, not the device). Phase
 * 3.E lands that plumbing as part of the pipeline-layout wrapper.
 * The resolver is left in place but currently unused — callers into
 * _bind_texture hit the "pfn still NULL → record-only" warning path
 * and log their way forward. The attribute silences -Wunused on
 * aggressive toolchains (Alpine gcc 14 with Wextra). */
static PFN_vkCmdPushDescriptorSetKHR
resolve_push_desc(struct lagfx_vk_state *vk) __attribute__((unused));

static PFN_vkCmdPushDescriptorSetKHR
resolve_push_desc(struct lagfx_vk_state *vk) {
    if (!vk || vk->device == VK_NULL_HANDLE) {
        return NULL;
    }
    if (vk->push_desc_pfn != NULL) {
        return vk->push_desc_pfn;
    }
    vk->push_desc_pfn =
        (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(
            vk->device, "vkCmdPushDescriptorSetKHR");
    return vk->push_desc_pfn;
}

/* --- Argument-validation helper ------------------------------ */

static bool state_in_pass(const lagfx_translate_render_state_t *state) {
    if (!state) {
        LAGFX_ERR("translate_render: NULL state");
        return false;
    }
    if (!state->in_pass) {
        LAGFX_ERR("translate_render: no active render pass "
                  "(call _begin first)");
        return false;
    }
    return true;
}

/* --- Public API --------------------------------------------- */

lagfx_status_t lagfx_translate_render_begin(
    struct lagfx_vk_state *vk,
    lagfx_translate_render_state_t *state,
    VkCommandBuffer vk_cmdbuf,
    VkImage target,
    uint32_t target_width,
    uint32_t target_height,
    const float clear_rgba[4],
    const VkRenderingAttachmentInfoKHR *color_attachments,
    uint32_t color_attachment_count) {
    if (!vk || !state || vk_cmdbuf == VK_NULL_HANDLE
        || target_width == 0u || target_height == 0u) {
        LAGFX_ERR("translate_render_begin: invalid arg");
        return LAGFX_ERR_INVALID_ARG;
    }
    if (!vk->initialized) {
        LAGFX_ERR("translate_render_begin: vk state not initialized");
        return LAGFX_ERR_BACKEND;
    }
    if (!vk->have_dynamic_rendering) {
        LAGFX_ERR("translate_render_begin: VK_KHR_dynamic_rendering "
                   "required by Phase 3.A encoder");
        return LAGFX_ERR_BACKEND;
    }
    if (state->in_pass) {
        LAGFX_ERR("translate_render_begin: already in pass "
                   "(missing _end?)");
        return LAGFX_ERR_INVALID_ARG;
    }

    memset(state, 0, sizeof(*state));

    state->cmdbuf         = vk_cmdbuf;
    state->target_image   = target;
    state->target_width   = target_width;
    state->target_height  = target_height;
    state->in_pass        = true;
    state->vk             = vk;

    if (target == VK_NULL_HANDLE) {
        if (vk->frame_image_view != VK_NULL_HANDLE) {
            state->target_image = vk->frame_image;
            LAGFX_LOG("translate_render_begin: using default frame image %p "
                      "as target", (void *)vk->frame_image);
        } else {
            LAGFX_LOG("translate_render_begin: target=NULL and no frame image, "
                      "skipping vkCmdBeginRendering (state-only) %ux%u",
                      target_width, target_height);
            return LAGFX_OK;
        }
    }

    VkRenderingAttachmentInfoKHR fallback_att = {
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = VK_NULL_HANDLE,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue  = { .color = { .float32 = {
            clear_rgba ? clear_rgba[0] : 0.0f,
            clear_rgba ? clear_rgba[1] : 0.0f,
            clear_rgba ? clear_rgba[2] : 0.0f,
            clear_rgba ? clear_rgba[3] : 1.0f,
        } } },
    };

    const VkRenderingAttachmentInfoKHR *p_atts;
    uint32_t                            n_atts;
    if (color_attachments != NULL && color_attachment_count > 0u) {
        p_atts = color_attachments;
        n_atts = color_attachment_count;
    } else {
        if (vk->frame_image_view != VK_NULL_HANDLE) {
            fallback_att.imageView = vk->frame_image_view;
        }
        p_atts = &fallback_att;
        n_atts = 1u;
    }

    VkRenderingInfo ri = {
        .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea           = {
            .offset = { 0, 0 },
            .extent = { target_width, target_height },
        },
        .layerCount           = 1,
        .colorAttachmentCount = n_atts,
        .pColorAttachments    = p_atts,
    };
    vkCmdBeginRendering(vk_cmdbuf, &ri);

    state->render_pass_active = true;

    LAGFX_LOG("translate_render_begin: target=%p %ux%u atts=%u",
              (void *)target, target_width, target_height, n_atts);
    return LAGFX_OK;
}

lagfx_status_t lagfx_translate_render_bind_pipeline(
    lagfx_translate_render_state_t *state,
    lagfx_shader_kind_t shader_kind,
    VkPipelineLayout layout) {
    if (!state_in_pass(state)) {
        return LAGFX_ERR_INVALID_ARG;
    }
    state->layout         = layout;
    state->shader_kind    = (uint32_t)shader_kind;
    state->pipeline_bound = true;

    if (state->render_pass_active && state->cmdbuf != VK_NULL_HANDLE) {
        struct lagfx_vk_state *vk = state->vk;
        VkPipeline pipe = VK_NULL_HANDLE;
        VkPipelineLayout lay = layout;

        if (vk && vk->passthrough_pipeline != VK_NULL_HANDLE) {
            pipe = vk->passthrough_pipeline;
            if (lay == VK_NULL_HANDLE) {
                lay = vk->passthrough_layout;
            }
        }

        if (pipe != VK_NULL_HANDLE) {
            vkCmdBindPipeline(state->cmdbuf,
                              VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            state->layout = lay;
            if (vk->fallback_desc_set != VK_NULL_HANDLE) {
                vkCmdBindDescriptorSets(state->cmdbuf,
                                        VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        lay, 0, 1,
                                        &vk->fallback_desc_set, 0, NULL);
            }
            LAGFX_LOG("translate_render_bind_pipeline: bound passthrough "
                      "pipeline %p layout=%p ds=%p (kind=%u)",
                      (void *)pipe, (void *)lay,
                      (void *)vk->fallback_desc_set,
                      (unsigned)shader_kind);
        } else {
            LAGFX_LOG("translate_render_bind_pipeline: kind=%u layout=%p "
                      "(no passthrough pipeline — record-only)",
                      (unsigned)shader_kind, (void *)layout);
        }
    } else {
        LAGFX_LOG("translate_render_bind_pipeline: kind=%u layout=%p "
                  "(state-only, no Vulkan render pass active)",
                  (unsigned)shader_kind, (void *)layout);
    }
    return LAGFX_OK;
}

lagfx_status_t lagfx_translate_render_bind_texture(
    lagfx_translate_render_state_t *state,
    uint32_t binding,
    VkImageView view,
    VkSampler sampler) {
    if (!state_in_pass(state)) {
        return LAGFX_ERR_INVALID_ARG;
    }
    if (!state->pipeline_bound) {
        LAGFX_ERR("translate_render_bind_texture: no pipeline bound "
                  "(call _bind_pipeline first)");
        return LAGFX_ERR_INVALID_ARG;
    }
    if (view == VK_NULL_HANDLE) {
        LAGFX_ERR("translate_render_bind_texture: view=NULL");
        return LAGFX_ERR_INVALID_ARG;
    }
    if (state->layout == VK_NULL_HANDLE) {
        /* Layout has to be known to push a descriptor — on the
         * Phase 3.A scaffold path where Phase 3.E hasn't built it
         * yet, we warn and no-op. Tests that pass a real layout
         * take the full path. */
        LAGFX_WARN("translate_render_bind_texture: layout=NULL "
                   "(Phase 3.A scaffold — push skipped) "
                   "pipeline not yet bound");
        return LAGFX_OK;
    }

    /* The render state carries a back-pointer to the lagfx_vk_state
     * that owns the VkDevice + layout. Use it to resolve / read the
     * cached vkCmdPushDescriptorSetKHR entry point. Degrade to
     * "record-only" if the resolver returns NULL (extension missing
     * or device not yet ready). */
    PFN_vkCmdPushDescriptorSetKHR pfn =
        state->vk ? state->vk->push_desc_pfn : NULL;
    if (pfn == NULL) {
        LAGFX_WARN("translate_render_bind_texture: "
                   "vkCmdPushDescriptorSetKHR not resolved yet "
                   "(Phase 3.A scaffold — record only)");
        return LAGFX_OK;
    }

    VkDescriptorImageInfo img = {
        .sampler     = sampler,
        .imageView   = view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    /* Descriptor-type choice: combined sampler matches the GLSL
     * sampler2D bindings in our 5-shader catalog. Cursor's UBO
     * binding (set=0,binding=0) is a different type and is pushed
     * by a separate (future) _bind_uniform_buffer entry point
     * tracked in FIXME(phase-3e-ubo-bind). */
    VkWriteDescriptorSet write = {
        .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstBinding      = binding,
        .descriptorCount = 1,
        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo      = &img,
    };
    pfn(state->cmdbuf,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        state->layout,
        /* set = */ 0,
        /* writeCount = */ 1,
        &write);

    LAGFX_LOG("translate_render_bind_texture: binding=%u view=%p "
              "sampler=%p", binding, (void *)view, (void *)sampler);
    return LAGFX_OK;
}

lagfx_status_t lagfx_translate_render_draw(
    lagfx_translate_render_state_t *state,
    uint32_t vertex_count,
    uint32_t instance_count,
    uint32_t first_vertex,
    uint32_t first_instance) {
    if (!state_in_pass(state)) {
        return LAGFX_ERR_INVALID_ARG;
    }
    if (!state->pipeline_bound) {
        LAGFX_ERR("translate_render_draw: no pipeline bound");
        return LAGFX_ERR_INVALID_ARG;
    }
    if (vertex_count == 0u || instance_count == 0u) {
        LAGFX_ERR("translate_render_draw: zero counts "
                  "(v=%u i=%u)", vertex_count, instance_count);
        return LAGFX_ERR_INVALID_ARG;
    }

    if (state->render_pass_active && state->cmdbuf != VK_NULL_HANDLE) {
        if (state->vk && state->vk->dummy_vb != VK_NULL_HANDLE) {
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(state->cmdbuf, 0, 1,
                                   &state->vk->dummy_vb, &offset);
        }
        vkCmdDraw(state->cmdbuf, vertex_count, instance_count,
                  first_vertex, first_instance);
    }

    LAGFX_LOG("translate_render_draw: v=%u i=%u firstV=%u firstI=%u "
              "(kind=%u)",
              vertex_count, instance_count,
              first_vertex, first_instance, state->shader_kind);
    return LAGFX_OK;
}

lagfx_status_t lagfx_translate_render_draw_indexed(
    lagfx_translate_render_state_t *state,
    uint32_t index_count,
    uint32_t instance_count,
    uint32_t first_index,
    int32_t vertex_offset,
    uint32_t first_instance) {
    if (!state_in_pass(state)) {
        return LAGFX_ERR_INVALID_ARG;
    }
    if (!state->pipeline_bound) {
        LAGFX_ERR("translate_render_draw_indexed: no pipeline bound");
        return LAGFX_ERR_INVALID_ARG;
    }
    if (index_count == 0u || instance_count == 0u) {
        LAGFX_ERR("translate_render_draw_indexed: zero counts "
                  "(idx=%u inst=%u)", index_count, instance_count);
        return LAGFX_ERR_INVALID_ARG;
    }

    if (state->render_pass_active && state->cmdbuf != VK_NULL_HANDLE) {
        if (state->vk && state->vk->dummy_vb != VK_NULL_HANDLE) {
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(state->cmdbuf, 0, 1,
                                   &state->vk->dummy_vb, &offset);
        }
        vkCmdDrawIndexed(state->cmdbuf, index_count, instance_count,
                         first_index, vertex_offset, first_instance);
    }

    LAGFX_LOG("translate_render_draw_indexed: idx=%u inst=%u "
              "firstIdx=%u vtxOff=%d firstInst=%u (kind=%u)",
              index_count, instance_count,
              first_index, vertex_offset, first_instance,
              state->shader_kind);
    return LAGFX_OK;
}

lagfx_status_t lagfx_translate_render_end(
    lagfx_translate_render_state_t *state) {
    if (!state_in_pass(state)) {
        return LAGFX_ERR_INVALID_ARG;
    }
    if (state->render_pass_active) {
        vkCmdEndRendering(state->cmdbuf);
    }
    LAGFX_LOG("translate_render_end");
    state->in_pass           = false;
    state->pipeline_bound    = false;
    state->render_pass_active = false;
    return LAGFX_OK;
}

lagfx_status_t lagfx_translate_render_set_viewport(
    lagfx_translate_render_state_t *state,
    const VkViewport *viewport) {
    if (!state || !viewport) {
        return LAGFX_ERR_INVALID_ARG;
    }
    state->viewport     = *viewport;
    state->viewport_set = true;
    if (state->render_pass_active && state->cmdbuf != VK_NULL_HANDLE) {
        vkCmdSetViewport(state->cmdbuf, 0, 1, viewport);
    }
    LAGFX_TRACE("translate_render_set_viewport: x=%g y=%g w=%g h=%g",
                viewport->x, viewport->y,
                viewport->width, viewport->height);
    return LAGFX_OK;
}

lagfx_status_t lagfx_translate_render_set_scissor(
    lagfx_translate_render_state_t *state,
    const VkRect2D *scissor) {
    if (!state || !scissor) {
        return LAGFX_ERR_INVALID_ARG;
    }
    state->scissor     = *scissor;
    state->scissor_set = true;
    if (state->render_pass_active && state->cmdbuf != VK_NULL_HANDLE) {
        vkCmdSetScissor(state->cmdbuf, 0, 1, scissor);
    }
    LAGFX_TRACE("translate_render_set_scissor: x=%d y=%d %ux%u",
                scissor->offset.x, scissor->offset.y,
                scissor->extent.width, scissor->extent.height);
    return LAGFX_OK;
}

lagfx_status_t lagfx_translate_render_set_blend_color(
    lagfx_translate_render_state_t *state,
    const float rgba[4]) {
    if (!state || !rgba) {
        return LAGFX_ERR_INVALID_ARG;
    }
    memcpy(state->blend_color, rgba, sizeof(state->blend_color));
    state->blend_color_set = true;
    if (state->render_pass_active && state->cmdbuf != VK_NULL_HANDLE) {
        vkCmdSetBlendConstants(state->cmdbuf, rgba);
    }
    LAGFX_TRACE("translate_render_set_blend_color: r=%g g=%g b=%g a=%g",
                (double)rgba[0], (double)rgba[1],
                (double)rgba[2], (double)rgba[3]);
    return LAGFX_OK;
}

lagfx_status_t lagfx_translate_render_set_stencil_ref(
    lagfx_translate_render_state_t *state,
    uint32_t front_ref,
    uint32_t back_ref) {
    if (!state) {
        return LAGFX_ERR_INVALID_ARG;
    }
    state->stencil_front_ref = front_ref;
    state->stencil_back_ref  = back_ref;
    if (state->render_pass_active && state->cmdbuf != VK_NULL_HANDLE) {
        vkCmdSetStencilReference(state->cmdbuf,
                                 VK_STENCIL_FRONT_AND_BACK,
                                 front_ref);
    }
    LAGFX_TRACE("translate_render_set_stencil_ref: front=%u back=%u",
                front_ref, back_ref);
    return LAGFX_OK;
}

lagfx_status_t lagfx_translate_render_set_depth_bias(
    lagfx_translate_render_state_t *state,
    float depth_bias_constant,
    float depth_bias_clamp,
    float depth_bias_slope_factor) {
    if (!state) {
        return LAGFX_ERR_INVALID_ARG;
    }
    state->depth_bias_constant     = depth_bias_constant;
    state->depth_bias_clamp        = depth_bias_clamp;
    state->depth_bias_slope_factor = depth_bias_slope_factor;
    if (state->render_pass_active && state->cmdbuf != VK_NULL_HANDLE) {
        vkCmdSetDepthBias(state->cmdbuf,
                          depth_bias_constant,
                          depth_bias_clamp,
                          depth_bias_slope_factor);
    }
    LAGFX_TRACE("translate_render_set_depth_bias: bias=%g clamp=%g slope=%g",
                (double)depth_bias_constant, (double)depth_bias_clamp,
                (double)depth_bias_slope_factor);
    return LAGFX_OK;
}

lagfx_status_t lagfx_translate_render_set_depth_stencil_state(
    lagfx_translate_render_state_t *state,
    uint32_t ref) {
    if (!state) {
        return LAGFX_ERR_INVALID_ARG;
    }
    state->bound_depth_stencil_state_ref = ref;
    LAGFX_TRACE("translate_render_set_depth_stencil_state: ref=0x%08x", ref);
    return LAGFX_OK;
}

lagfx_status_t lagfx_translate_render_set_depth_clip_mode(
    lagfx_translate_render_state_t *state,
    uint32_t mode) {
    if (!state) {
        return LAGFX_ERR_INVALID_ARG;
    }
    state->depth_clip_mode = mode;
    LAGFX_TRACE("translate_render_set_depth_clip_mode: mode=%u", mode);
    return LAGFX_OK;
}

lagfx_status_t lagfx_translate_render_set_front_facing_winding(
    lagfx_translate_render_state_t *state,
    uint32_t value) {
    if (!state) {
        return LAGFX_ERR_INVALID_ARG;
    }
    state->front_facing_winding = value;
    LAGFX_TRACE("translate_render_set_front_facing_winding: value=%u", value);
    return LAGFX_OK;
}

lagfx_status_t lagfx_translate_render_set_cull_mode(
    lagfx_translate_render_state_t *state,
    uint32_t value) {
    if (!state) {
        return LAGFX_ERR_INVALID_ARG;
    }
    state->cull_mode = value;
    LAGFX_TRACE("translate_render_set_cull_mode: value=%u", value);
    return LAGFX_OK;
}

#else /* !LAGFX_HAVE_VULKAN --------------------------------- */

/* No-vulkan stubs: every entry point returns LAGFX_ERR_BACKEND so
 * callers that mistakenly drive the encoder on the Darwin path get
 * a clear signal rather than silent no-op. This matches the
 * render_target.c convention. */

lagfx_status_t lagfx_translate_render_begin(
    void *vk, lagfx_translate_render_state_t *state,
    void *vk_cmdbuf, void *target,
    uint32_t target_width, uint32_t target_height,
    const float clear_rgba[4],
    const void *color_attachments, uint32_t color_attachment_count) {
    (void)vk; (void)state; (void)vk_cmdbuf; (void)target;
    (void)target_width; (void)target_height; (void)clear_rgba;
    (void)color_attachments; (void)color_attachment_count;
    return LAGFX_ERR_BACKEND;
}

lagfx_status_t lagfx_translate_render_bind_pipeline(
    lagfx_translate_render_state_t *state,
    lagfx_shader_kind_t shader_kind, void *layout) {
    (void)state; (void)shader_kind; (void)layout;
    return LAGFX_ERR_BACKEND;
}

lagfx_status_t lagfx_translate_render_bind_texture(
    lagfx_translate_render_state_t *state,
    uint32_t binding, void *view, void *sampler) {
    (void)state; (void)binding; (void)view; (void)sampler;
    return LAGFX_ERR_BACKEND;
}

lagfx_status_t lagfx_translate_render_draw(
    lagfx_translate_render_state_t *state,
    uint32_t vertex_count, uint32_t instance_count,
    uint32_t first_vertex, uint32_t first_instance) {
    (void)state; (void)vertex_count; (void)instance_count;
    (void)first_vertex; (void)first_instance;
    return LAGFX_ERR_BACKEND;
}

lagfx_status_t lagfx_translate_render_draw_indexed(
    lagfx_translate_render_state_t *state,
    uint32_t index_count, uint32_t instance_count,
    uint32_t first_index, int32_t vertex_offset,
    uint32_t first_instance) {
    (void)state; (void)index_count; (void)instance_count;
    (void)first_index; (void)vertex_offset; (void)first_instance;
    return LAGFX_ERR_BACKEND;
}

lagfx_status_t lagfx_translate_render_end(
    lagfx_translate_render_state_t *state) {
    (void)state;
    return LAGFX_ERR_BACKEND;
}

lagfx_status_t lagfx_translate_render_set_viewport(
    lagfx_translate_render_state_t *state,
    const void *viewport) {
    (void)state; (void)viewport;
    return LAGFX_ERR_BACKEND;
}

lagfx_status_t lagfx_translate_render_set_scissor(
    lagfx_translate_render_state_t *state,
    const void *scissor) {
    (void)state; (void)scissor;
    return LAGFX_ERR_BACKEND;
}

lagfx_status_t lagfx_translate_render_set_blend_color(
    lagfx_translate_render_state_t *state,
    const float rgba[4]) {
    (void)state; (void)rgba;
    return LAGFX_ERR_BACKEND;
}

lagfx_status_t lagfx_translate_render_set_stencil_ref(
    lagfx_translate_render_state_t *state,
    uint32_t front_ref, uint32_t back_ref) {
    (void)state; (void)front_ref; (void)back_ref;
    return LAGFX_ERR_BACKEND;
}

lagfx_status_t lagfx_translate_render_set_depth_bias(
    lagfx_translate_render_state_t *state,
    float depth_bias_constant, float depth_bias_clamp,
    float depth_bias_slope_factor) {
    (void)state; (void)depth_bias_constant; (void)depth_bias_clamp;
    (void)depth_bias_slope_factor;
    return LAGFX_ERR_BACKEND;
}

lagfx_status_t lagfx_translate_render_set_depth_stencil_state(
    lagfx_translate_render_state_t *state,
    uint32_t ref) {
    (void)state; (void)ref;
    return LAGFX_ERR_BACKEND;
}

lagfx_status_t lagfx_translate_render_set_depth_clip_mode(
    lagfx_translate_render_state_t *state,
    uint32_t mode) {
    (void)state; (void)mode;
    return LAGFX_ERR_BACKEND;
}

lagfx_status_t lagfx_translate_render_set_front_facing_winding(
    lagfx_translate_render_state_t *state,
    uint32_t value) {
    (void)state; (void)value;
    return LAGFX_ERR_BACKEND;
}

lagfx_status_t lagfx_translate_render_set_cull_mode(
    lagfx_translate_render_state_t *state,
    uint32_t value) {
    (void)state; (void)value;
    return LAGFX_ERR_BACKEND;
}

#endif /* LAGFX_HAVE_VULKAN */

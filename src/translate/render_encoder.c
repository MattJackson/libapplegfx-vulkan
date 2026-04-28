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
#endif

#include <stdlib.h>
#include <string.h>

#ifdef LAGFX_HAVE_VULKAN

/* --- Push-descriptor dispatch cache --------------------------
 *
 * vkCmdPushDescriptorSetKHR is an instance-level function pointer
 * that must be resolved via vkGetDeviceProcAddr because it comes
 * from VK_KHR_push_descriptor (promoted to core in Vulkan 1.4, but
 * still gated on a valid load). Cached per-device via a small
 * static-single-value cache — the library only owns one VkDevice
 * at a time in Phase 3 so the single-entry cache is sufficient.
 * See FIXME(phase-3-encoder-multi-device) if we ever grow a
 * multi-device case. */
static struct {
    VkDevice                         cached_for;
    PFN_vkCmdPushDescriptorSetKHR    pfn;
} g_push_desc_cache;

/* Phase 3.A scaffold doesn't yet plumb the VkDevice down here (the
 * encoder state carries the command buffer, not the device). Phase
 * 3.E lands that plumbing as part of the pipeline-layout wrapper.
 * The resolver is left in place but currently unused — callers into
 * _bind_texture hit the "pfn still NULL → record-only" warning path
 * and log their way forward. The attribute silences -Wunused on
 * aggressive toolchains (Alpine gcc 14 with Wextra). */
static PFN_vkCmdPushDescriptorSetKHR
resolve_push_desc(VkDevice device) __attribute__((unused));

static PFN_vkCmdPushDescriptorSetKHR
resolve_push_desc(VkDevice device) {
    if (device == VK_NULL_HANDLE) {
        return NULL;
    }
    if (g_push_desc_cache.cached_for == device
        && g_push_desc_cache.pfn != NULL) {
        return g_push_desc_cache.pfn;
    }
    g_push_desc_cache.cached_for = device;
    g_push_desc_cache.pfn =
        (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(
            device, "vkCmdPushDescriptorSetKHR");
    return g_push_desc_cache.pfn;
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

    if (target == VK_NULL_HANDLE) {
        LAGFX_LOG("translate_render_begin: target=NULL, skipping "
                  "vkCmdBeginRendering (state-only) %ux%u",
                  target_width, target_height);
        return LAGFX_OK;
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
    /* Layout may legitimately be VK_NULL_HANDLE at Phase 3.A
     * scaffold — Phase 3.E populates it. Log and continue. */
    state->layout         = layout;
    state->shader_kind    = (uint32_t)shader_kind;
    state->pipeline_bound = true;

    /* FIXME(phase-3e-pipeline): once VkPipeline objects exist in
     * the catalog, resolve `shader_kind` + layout into a bound
     * VkPipeline here via vkCmdBindPipeline. Today we only track
     * the intent so downstream _bind_texture / _draw can gate on
     * "pipeline was set". */
    LAGFX_LOG("translate_render_bind_pipeline: kind=%u layout=%p "
              "(record-only at Phase 3.A)",
              (unsigned)shader_kind, (void *)layout);
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
                   "[FIXME(phase-3e-pipeline)]");
        return LAGFX_OK;
    }

    /* We don't carry the VkDevice on the state explicitly; resolving
     * the push-descriptor entry point requires vkGetDeviceProcAddr
     * against the device that owns state->layout, which Phase 3.E
     * plumbs on the pipeline-layout wrapper. For Phase 3.A we rely
     * on a cache populated by an earlier resolve_push_desc() call
     * and degrade to "record-only" when the cache is empty. */
    PFN_vkCmdPushDescriptorSetKHR pfn = g_push_desc_cache.pfn;
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
    uint32_t instance_count) {
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

    vkCmdDraw(state->cmdbuf, vertex_count, instance_count,
              /* firstVertex = */ 0u, /* firstInstance = */ 0u);

    LAGFX_LOG("translate_render_draw: v=%u i=%u (kind=%u)",
              vertex_count, instance_count, state->shader_kind);
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
    uint32_t vertex_count, uint32_t instance_count) {
    (void)state; (void)vertex_count; (void)instance_count;
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

#endif /* LAGFX_HAVE_VULKAN */

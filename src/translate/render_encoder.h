/*
 * libapplegfx-vulkan — Metal-to-Vulkan render command encoder (Phase 3.A)
 * src/translate/render_encoder.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Internal header. Not installed.
 *
 * Phase 3.A translator: expands the "clear only" path at
 * src/vulkan/render_target.c into a full render-pass encoder that
 * can record arbitrary draw calls into a caller-owned
 * VkCommandBuffer. The encoder is stateful (bind_pipeline →
 * bind_texture → draw → end) but entirely bag-of-state; no hidden
 * globals. Callers live in src/protocol/ops_cmdbuf.c (Phase 3.B
 * decoder) — the encoder's job is to translate the decoded
 * MTLRenderCommandEncoder-like operations into Vulkan wire
 * commands.
 *
 * === Scope boundary vs render_target.c ==========================
 *
 * - render_target.c owns allocation + clear-only attachment
 *   plumbing. Phase 2.B shipped it.
 * - render_encoder.c owns per-frame render pass recording: the
 *   vkCmdBeginRendering / bind / draw / end sequence that
 *   eventually replaces the simple clear.
 * - Pipeline state objects (VkPipeline, VkPipelineLayout,
 *   VkDescriptorSetLayout) are Phase 3.E scope — the encoder
 *   expects them to be pre-built and handed in.
 *
 * === Concurrency ================================================
 *
 * Not thread-safe. A given lagfx_translate_render_state_t MUST be
 * driven by one thread at a time; concurrent uses need separate
 * states. Matches the single-threaded QEMU BQL-driven dispatch on
 * the wire path.
 *
 * === Graceful degradation =======================================
 *
 * When built without Vulkan (LAGFX_HAVE_VULKAN unset) the public
 * API is still declared so callers can compile; every function is
 * a no-op that returns LAGFX_ERR_BACKEND. This mirrors
 * render_target.h's policy for the Darwin dev path.
 */

#ifndef LIBAPPLEGFX_TRANSLATE_RENDER_ENCODER_H
#define LIBAPPLEGFX_TRANSLATE_RENDER_ENCODER_H

#include "libapplegfx-vulkan.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LAGFX_MAX_BOUND_VERTEX_BUFFERS   16u
#define LAGFX_MAX_BOUND_FRAGMENT_TEXTURES 32u
#define LAGFX_MAX_BOUND_FRAGMENT_SAMPLERS  32u
#define LAGFX_MAX_BOUND_FRAGMENT_BUFFERS   32u
#define LAGFX_MAX_BOUND_VERTEX_TEXTURES    32u

#ifdef LAGFX_HAVE_VULKAN
#  include <vulkan/vulkan.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifdef LAGFX_HAVE_VULKAN
/* Forward-decl so the begin() signature below can reference it
 * without dragging src/vulkan/instance.h through every consumer.
 * The real definition lives in src/vulkan/instance.h. */
struct lagfx_vk_state;
#endif

/* === Encoder state ===========================================
 *
 * Bag-of-state carried across begin → bind* → draw → end. Zeroed
 * on construction; caller owns the struct. Fields marked
 * "scratch" are maintained by the implementation and should not
 * be touched by callers.
 *
 * The struct is defined here (not behind a typedef-to-forward) so
 * callers can allocate on the stack. Size is bounded and small
 * (~128 B) because everything is handles.
 */
typedef struct lagfx_translate_render_state {
#ifdef LAGFX_HAVE_VULKAN
    /* Active command buffer — set by _begin, cleared by _end. Writes
     * go here via vkCmd* calls. */
    VkCommandBuffer cmdbuf;

    /* Back-reference to the Vulkan device state. Set by _begin so that
     * _bind_pipeline / _draw can access passthrough pipeline etc. */
    struct lagfx_vk_state *vk;

    /* Target attachment parameters — cached at _begin so _end /
     * _draw don't need them re-passed. */
    VkImage    target_image;
    uint32_t   target_width;
    uint32_t   target_height;

    /* Pipeline state — tracked, not yet resolved to a VkPipeline. */
    VkPipelineLayout layout;        /* from bind_pipeline */
    uint32_t         shader_kind;   /* lagfx_shader_kind_t cast to u32 */

    /* Dynamic state — set by set_viewport / set_scissor /
     * set_blend_color, applied into the command buffer immediately
     * when a render pass is active. */
    VkViewport viewport;
    VkRect2D   scissor;
    float      blend_color[4];
    bool       viewport_set;
    bool       scissor_set;
    bool       blend_color_set;

    /* Tracks whether vkCmdBeginRendering was actually called (vs
     * in_pass which tracks logical lifecycle). When target_image is
     * VK_NULL_HANDLE we skip the Vulkan render pass but still track
     * state. */
    bool render_pass_active;

    uint32_t stencil_front_ref;
    uint32_t stencil_back_ref;
    float    depth_bias_constant;
    float    depth_bias_clamp;
    float    depth_bias_slope_factor;
#else
    /* No-vulkan stub: a single field so sizeof(struct)>0 and callers
     * can still declare + zero-init. */
    int _placeholder;
#endif

    /* Rendering state flags. */
    bool in_pass;          /* between _begin and _end */
    bool pipeline_bound;   /* bind_pipeline was called since _begin */

    /* Protocol-level resource bindings — stored from opcode handlers
     * for future VkPipeline/VkBuffer/VkDescriptor resolution. */
    uint32_t bound_pipeline_ref;

    struct {
        uint32_t ref;
        uint64_t offset;
    } bound_vertex_buffers[LAGFX_MAX_BOUND_VERTEX_BUFFERS];
    uint32_t bound_vertex_buffer_count;
    uint32_t bound_vertex_buffer_first;

    uint32_t bound_fragment_textures[LAGFX_MAX_BOUND_FRAGMENT_TEXTURES];
    uint32_t bound_fragment_texture_count;
    uint32_t bound_fragment_texture_first;

    uint32_t bound_depth_stencil_state_ref;
    uint32_t depth_clip_mode;
    uint32_t front_facing_winding;
    uint32_t cull_mode;

    struct {
        uint32_t ref;
        uint64_t offset;
    } bound_fragment_buffers[LAGFX_MAX_BOUND_FRAGMENT_BUFFERS];
    uint32_t bound_fragment_buffer_count;
    uint32_t bound_fragment_buffer_first;

    uint32_t bound_fragment_samplers[LAGFX_MAX_BOUND_FRAGMENT_SAMPLERS];
    uint32_t bound_fragment_sampler_count;
    uint32_t bound_fragment_sampler_first;

    uint32_t bound_vertex_textures[LAGFX_MAX_BOUND_VERTEX_TEXTURES];
    uint32_t bound_vertex_texture_count;
    uint32_t bound_vertex_texture_first;

    struct {
        uint32_t ref;
        uint64_t offset;
        uint64_t stride;
    } bound_vertex_buffers_stride[LAGFX_MAX_BOUND_VERTEX_BUFFERS];
    uint32_t bound_vertex_buffers_stride_count;
    uint32_t bound_vertex_buffers_stride_first;
} lagfx_translate_render_state_t;

/* === Public API ===============================================
 *
 * All functions return LAGFX_OK on success, an LAGFX_ERR_* on
 * failure with a log line. Most failures are caller bugs (NULL
 * handle, out-of-sequence call) and are retained as hard errors
 * rather than promoted to warnings. */

#ifdef LAGFX_HAVE_VULKAN

/* Begin a render pass on `vk_cmdbuf` targeting `target` using
 * VK_KHR_dynamic_rendering (core in Vulkan 1.3). `color_attachments`
 * may be NULL, in which case the encoder fabricates a single color
 * attachment wrapping `target` with LOAD_OP_CLEAR + STORE_OP_STORE
 * and the clear colour from `clear_rgba`. When the caller passes
 * a non-NULL `color_attachments[]`, `color_attachment_count` >= 1
 * and `clear_rgba` is ignored (the caller owns load/store policy).
 *
 * Writes internal state into *state; caller allocates, zeroes on
 * first use. Requires vk->have_dynamic_rendering. */
lagfx_status_t lagfx_translate_render_begin(
    struct lagfx_vk_state *vk,
    lagfx_translate_render_state_t *state,
    VkCommandBuffer vk_cmdbuf,
    VkImage target,
    uint32_t target_width,
    uint32_t target_height,
    const float clear_rgba[4],
    const VkRenderingAttachmentInfoKHR *color_attachments,
    uint32_t color_attachment_count);

/* Bind a pipeline selected by `shader_kind`. At Phase 3.A scaffold
 * we only RECORD the intent — the catalog → VkShader/VkPipeline
 * binding is Phase 3.E's job. `layout` is stored for later
 * push-descriptor calls. Returns LAGFX_ERR_INVALID_ARG if called
 * outside a begin/end pair. */
lagfx_status_t lagfx_translate_render_bind_pipeline(
    lagfx_translate_render_state_t *state,
    lagfx_shader_kind_t shader_kind,
    VkPipelineLayout layout);

/* Bind a sampled-image descriptor at (set=0, binding=`binding`)
 * via vkCmdPushDescriptorSetKHR. Requires bind_pipeline first
 * (layout must be known). `sampler` may be VK_NULL_HANDLE only
 * if the pipeline's binding is declared with an immutable
 * sampler — the encoder does not validate that; it only records
 * the push. */
lagfx_status_t lagfx_translate_render_bind_texture(
    lagfx_translate_render_state_t *state,
    uint32_t binding,
    VkImageView view,
    VkSampler sampler);

/* Record a vkCmdDraw with the given vertex + instance counts.
 * Requires a bound pipeline. `first_vertex` and `first_instance`
 * map directly to the Vulkan parameters of the same names. */
lagfx_status_t lagfx_translate_render_draw(
    lagfx_translate_render_state_t *state,
    uint32_t vertex_count,
    uint32_t instance_count,
    uint32_t first_vertex,
    uint32_t first_instance);

/* Record a vkCmdDrawIndexed. Requires a bound pipeline. Maps
 * directly to the Vulkan vkCmdDrawIndexed parameters. */
lagfx_status_t lagfx_translate_render_draw_indexed(
    lagfx_translate_render_state_t *state,
    uint32_t index_count,
    uint32_t instance_count,
    uint32_t first_index,
    int32_t vertex_offset,
    uint32_t first_instance);

/* End the render pass (vkCmdEndRendering) and reset state for a
 * subsequent _begin. Does NOT end the underlying command buffer —
 * the caller owns begin/end on the VkCommandBuffer itself. */
lagfx_status_t lagfx_translate_render_end(
    lagfx_translate_render_state_t *state);

/* Set the viewport. If a render pass is active and the command buffer
 * is valid, records vkCmdSetViewport immediately. The viewport is
 * also stored for replay if the render pass begins later. */
lagfx_status_t lagfx_translate_render_set_viewport(
    lagfx_translate_render_state_t *state,
    const VkViewport *viewport);

/* Set the scissor rectangle. Records vkCmdSetScissor if active. */
lagfx_status_t lagfx_translate_render_set_scissor(
    lagfx_translate_render_state_t *state,
    const VkRect2D *scissor);

/* Set the blend constants. Records vkCmdSetBlendConstants if active. */
lagfx_status_t lagfx_translate_render_set_blend_color(
    lagfx_translate_render_state_t *state,
    const float rgba[4]);

lagfx_status_t lagfx_translate_render_set_stencil_ref(
    lagfx_translate_render_state_t *state,
    uint32_t front_ref,
    uint32_t back_ref);

lagfx_status_t lagfx_translate_render_set_depth_bias(
    lagfx_translate_render_state_t *state,
    float depth_bias_constant,
    float depth_bias_clamp,
    float depth_bias_slope_factor);

lagfx_status_t lagfx_translate_render_set_depth_stencil_state(
    lagfx_translate_render_state_t *state,
    uint32_t ref);

lagfx_status_t lagfx_translate_render_set_depth_clip_mode(
    lagfx_translate_render_state_t *state,
    uint32_t mode);

lagfx_status_t lagfx_translate_render_set_front_facing_winding(
    lagfx_translate_render_state_t *state,
    uint32_t value);

lagfx_status_t lagfx_translate_render_set_cull_mode(
    lagfx_translate_render_state_t *state,
    uint32_t value);

#else /* !LAGFX_HAVE_VULKAN ------------------------------------ */

/* Forward-decl-only stubs so callers can compile on the no-vulkan
 * (Darwin-dev) path. Define opaque handle types as void* so the
 * signatures remain valid without <vulkan/vulkan.h>. */
typedef void *lagfx_vk_image_stub_t;
typedef void *lagfx_vk_cmdbuf_stub_t;
typedef void *lagfx_vk_layout_stub_t;
typedef void *lagfx_vk_view_stub_t;
typedef void *lagfx_vk_sampler_stub_t;

lagfx_status_t lagfx_translate_render_begin(
    void *vk,
    lagfx_translate_render_state_t *state,
    lagfx_vk_cmdbuf_stub_t vk_cmdbuf,
    lagfx_vk_image_stub_t target,
    uint32_t target_width,
    uint32_t target_height,
    const float clear_rgba[4],
    const void *color_attachments,
    uint32_t color_attachment_count);

lagfx_status_t lagfx_translate_render_bind_pipeline(
    lagfx_translate_render_state_t *state,
    lagfx_shader_kind_t shader_kind,
    lagfx_vk_layout_stub_t layout);

lagfx_status_t lagfx_translate_render_bind_texture(
    lagfx_translate_render_state_t *state,
    uint32_t binding,
    lagfx_vk_view_stub_t view,
    lagfx_vk_sampler_stub_t sampler);

lagfx_status_t lagfx_translate_render_draw(
    lagfx_translate_render_state_t *state,
    uint32_t vertex_count,
    uint32_t instance_count,
    uint32_t first_vertex,
    uint32_t first_instance);

lagfx_status_t lagfx_translate_render_draw_indexed(
    lagfx_translate_render_state_t *state,
    uint32_t index_count,
    uint32_t instance_count,
    uint32_t first_index,
    int32_t vertex_offset,
    uint32_t first_instance);

lagfx_status_t lagfx_translate_render_end(
    lagfx_translate_render_state_t *state);

lagfx_status_t lagfx_translate_render_set_viewport(
    lagfx_translate_render_state_t *state,
    const void *viewport);

lagfx_status_t lagfx_translate_render_set_scissor(
    lagfx_translate_render_state_t *state,
    const void *scissor);

lagfx_status_t lagfx_translate_render_set_blend_color(
    lagfx_translate_render_state_t *state,
    const float rgba[4]);

lagfx_status_t lagfx_translate_render_set_stencil_ref(
    lagfx_translate_render_state_t *state,
    uint32_t front_ref, uint32_t back_ref);

lagfx_status_t lagfx_translate_render_set_depth_bias(
    lagfx_translate_render_state_t *state,
    float depth_bias_constant, float depth_bias_clamp,
    float depth_bias_slope_factor);

lagfx_status_t lagfx_translate_render_set_depth_stencil_state(
    lagfx_translate_render_state_t *state,
    uint32_t ref);

lagfx_status_t lagfx_translate_render_set_depth_clip_mode(
    lagfx_translate_render_state_t *state,
    uint32_t mode);

lagfx_status_t lagfx_translate_render_set_front_facing_winding(
    lagfx_translate_render_state_t *state,
    uint32_t value);

lagfx_status_t lagfx_translate_render_set_cull_mode(
    lagfx_translate_render_state_t *state,
    uint32_t value);

#endif /* LAGFX_HAVE_VULKAN */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBAPPLEGFX_TRANSLATE_RENDER_ENCODER_H */

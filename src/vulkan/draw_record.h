/*
 * libapplegfx-vulkan — Vulkan draw command recording (Step 4)
 * src/vulkan/draw_record.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Encapsulates the begin/bind/draw/end/submit bundle for one-shot
 * draw execution. Called by compute_inner_ops.c when a draw fires.
 */

#ifndef LIBAPPLEGFX_VULKAN_DRAW_RECORD_H
#define LIBAPPLEGFX_VULKAN_DRAW_RECORD_H

#include "libapplegfx-vulkan.h"
#include "render_target.h"  /* lagfx_vk_render_target_t */

#ifdef LAGFX_HAVE_VULKAN
#  include <vulkan/vulkan.h>
#endif

#ifdef LAGFX_HAVE_VULKAN
/* Record and submit a single draw command into a one-shot command buffer.
 *
 * Parameters:
 *   vk            — lagfx_vk_state with cmd_pool, graphics_queue, device
 *   pipeline      — VkPipeline already built by Step 3
 *   rt            — render target whose image is the destination; used for
 *                   layout transitions and dynamic rendering attachment
 *   indexed       — true for indexed draw (vkCmdDrawIndexed), false for
 *                   unindexed (vkCmdDraw)
 *   vertex_count  — count for vkCmdDraw, or index_count for vkCmdDrawIndexed
 *   instance_count — always 1 for current step (non-instanced path)
 *   first_vertex  — base VertexIndex for vkCmdDraw
 *   first_instance — always 0 for current step
 *   index_buffer_ref — ignored when indexed=false; will be used in future
 *                      stages to bind the actual VkBuffer
 *
 * Returns LAGFX_OK on success, LAGFX_ERR_BACKEND on Vulkan failure.
 *
 * Anti-fab citations:
 *   - vkAllocateCommandBuffers: triangle-lavapipe-e2e.c:476-480
 *   - vkBeginCommandBuffer (ONE_TIME_SUBMIT): triangle-lavapipe-e2e.c:483-487
 *   - vkCmdBeginRendering (dynamic rendering): display.c:519-530, render_target.c:364-369
 *   - vkCmdBindPipeline: triangle-lavapipe-e2e.c:500
 *   - vkCmdDraw: triangle-lavapipe-e2e.c:501 (unindexed), display.c:527 (via cursor)
 *   - vkCmdEndRendering: display.c:528, render_target.c:369
 *   - vkQueueSubmit + vkWaitForFences: triangle-lavapipe-e2e.c:504-513
 */
lagfx_status_t lagfx_vk_draw_record_and_submit(
    struct lagfx_vk_state *vk,
    VkPipeline pipeline,
    lagfx_vk_render_target_t *rt,
    bool indexed,
    uint32_t vertex_count,
    uint32_t instance_count,
    int32_t first_vertex,
    uint32_t first_instance,
    uint32_t index_buffer_ref);

/* Stage 85b — same, but binds `desc_set` (set 0) via `pipe_layout` before the
 * draw, for translated resource-using pipelines. Pass VK_NULL_HANDLE for both
 * to behave like the plain entry above. */
lagfx_status_t lagfx_vk_draw_record_and_submit_bound(
    struct lagfx_vk_state *vk,
    VkPipeline pipeline,
    VkPipelineLayout pipe_layout,
    VkDescriptorSet desc_set,
    lagfx_vk_render_target_t *rt,
    bool indexed,
    uint32_t vertex_count,
    uint32_t instance_count,
    int32_t first_vertex,
    uint32_t first_instance,
    uint32_t index_buffer_ref,
    VkBuffer vertex_buffer);   /* VK_NULL_HANDLE = no vertex-input binding */
#endif /* LAGFX_HAVE_VULKAN */

#endif /* LIBAPPLEGFX_VULKAN_DRAW_RECORD_H */

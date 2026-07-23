/*
 * libapplegfx-vulkan — Vulkan draw command recording (Step 4)
 * src/vulkan/draw_record.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Implementation of lagfx_vk_draw_record_and_submit. Encapsulates the
 * one-shot draw path: allocate CB, begin, transition image layout,
 * begin rendering with dynamic rendering, bind pipeline, record draw,
 * end rendering, end CB, submit, wait for fence, free CB.
 */

#include "draw_record.h"
#include "command.h"
#include "common/log.h"
#include "common/perf.h"
#include <stdlib.h>

#ifdef LAGFX_HAVE_VULKAN

/* Thin wrapper: the original substitute-path entry (no descriptor set). */
lagfx_status_t lagfx_vk_draw_record_and_submit(
    struct lagfx_vk_state *vk,
    VkPipeline pipeline,
    lagfx_vk_render_target_t *rt,
    bool indexed,
    uint32_t vertex_count,
    uint32_t instance_count,
    int32_t first_vertex,
    uint32_t first_instance,
    uint32_t index_buffer_ref) {
    return lagfx_vk_draw_record_and_submit_bound(
        vk, pipeline, VK_NULL_HANDLE, VK_NULL_HANDLE, rt, indexed,
        vertex_count, instance_count, first_vertex, first_instance,
        index_buffer_ref, VK_NULL_HANDLE, NULL);
}

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
    VkBuffer vertex_buffer,
    const lagfx_draw_region_t *region) {

    (void)index_buffer_ref;  /* TODO: bind actual VkBuffer in future stage */
    
    if (!vk || !vk->initialized || vk->device == VK_NULL_HANDLE
        || vk->graphics_queue == VK_NULL_HANDLE || vk->cmd_pool == VK_NULL_HANDLE) {
        LAGFX_ERR("draw_record_and_submit: Vulkan state not fully initialized");
        return LAGFX_ERR_BACKEND;
    }
    
    if (pipeline == VK_NULL_HANDLE || rt == NULL || rt->image == VK_NULL_HANDLE) {
        LAGFX_ERR("draw_record_and_submit: pipeline=%p rt=%p rt->image=%p",
                  (void *)pipeline, (void *)rt, (void *)rt->image);
        return LAGFX_ERR_INVALID_ARG;
    }
    
    /* Allocate one-shot command buffer */
    VkCommandBuffer cb = VK_NULL_HANDLE;
    lagfx_status_t st = lagfx_vk_cmdbuf_alloc(vk, &cb);
    if (st != LAGFX_OK) {
        return st;
    }
    
    /* Begin recording (one-time submit pattern from triangle-lavapipe-e2e.c:483-487) */
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VkResult vr = vkBeginCommandBuffer(cb, &bi);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("draw_record_and_submit: vkBeginCommandBuffer failed (%d)", (int)vr);
        lagfx_vk_cmdbuf_free(vk, cb);
        return LAGFX_ERR_BACKEND;
    }
    
    /* Transition render target to COLOR_ATTACHMENT_OPTIMAL for rendering */
    VkAccessFlags src_access = 0;
    VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    if (rt->layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        src_access = VK_ACCESS_TRANSFER_READ_BIT;
        src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (rt->layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        /* B10: a prior draw left the RT in color-attachment layout — the
         * WAW hazard against that prior color write must be covered, or a
         * real GPU (lavapipe hides it) shows ordering artifacts. */
        src_access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        src_stage  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    }
    
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = rt->layout,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = rt->image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    vkCmdPipelineBarrier(cb, src_stage, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, NULL, 0, NULL, 1, &barrier);
    rt->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    
    /* Begin dynamic rendering with color attachment (pattern from display.c:519-530) */
    VkRenderingAttachmentInfo color_att = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = rt->view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,  /* preserve existing content */
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    };

    /* M1 diagnostic (env-gated): clear to a distinctive teal before this draw.
     * If the isolated translated frame turns teal, the zero-resource-data draws
     * are DEGENERATE (positions collapse → nothing rasterizes) and a correct
     * clear/non-zero data would be visible — i.e. a non-black frame is reachable
     * within M1. If it stays black, the draws cover the RT with black geometry
     * and visible content genuinely needs M2 resource data. Decisive either way. */
    if (getenv("LAGFX_TEST_CLEAR") != NULL) {
        color_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color_att.clearValue.color.float32[0] = 0.0f;
        color_att.clearValue.color.float32[1] = 0.6f;
        color_att.clearValue.color.float32[2] = 0.6f;
        color_att.clearValue.color.float32[3] = 1.0f;
    }

    VkRenderingInfo ri = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {{0, 0}, {rt->width, rt->height}},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_att,
    };
    vkCmdBeginRendering(cb, &ri);
    
    /* Bind the pipeline (pattern from triangle-lavapipe-e2e.c:500) */
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    /* Dynamic viewport/scissor sized to THIS render target (the pipeline no
     * longer bakes 1920×1080 — that mis-scaled the 1280×1024 per-pass draws).
     * Y-FLIP via negative height (Metal Y-up → Vulkan Y-down); kill-switch
     * LAGFX_DISABLE_YFLIP. */
    {
        bool yflip = (getenv("LAGFX_DISABLE_YFLIP") == NULL);
        /* Guest region (KICKOFF-shading-throughput): CA transforms vertices
         * for the DECLARED viewport rect (pixel->NDC matrix = 2/vp_w etc.)
         * and clips with the declared scissor. Mapping NDC onto the full RT
         * stretched small layers across the whole target, and the missing
         * scissor made full-target wash triangles shade EVERY RT pixel
         * (2M-8M px of the 73KB Xgc ubershader on lavapipe = the 20-30s
         * "runaway" draws). Clamp both rects to the RT; zero width = unset. */
        uint32_t vx = 0, vy = 0, vw = rt->width, vh = rt->height;
        uint32_t sx = 0, sy = 0, sw = rt->width, sh = rt->height;
        if (region && region->vp_w && region->vp_x < rt->width
            && region->vp_y < rt->height) {
            vx = region->vp_x; vy = region->vp_y;
            vw = region->vp_w; vh = region->vp_h;
            if (vx + vw > rt->width)  vw = rt->width - vx;
            if (vy + vh > rt->height) vh = rt->height - vy;
        }
        if (region && region->sc_w && region->sc_x < rt->width
            && region->sc_y < rt->height) {
            sx = region->sc_x; sy = region->sc_y;
            sw = region->sc_w; sh = region->sc_h;
            if (sx + sw > rt->width)  sw = rt->width - sx;
            if (sy + sh > rt->height) sh = rt->height - sy;
        }
        VkViewport dvp = {
            .x = (float)vx, .y = yflip ? (float)(vy + vh) : (float)vy,
            .width = (float)vw,
            .height = yflip ? -(float)vh : (float)vh,
            .minDepth = 0.0f, .maxDepth = 1.0f,
        };
        VkRect2D dsc = { .offset = {(int32_t)sx, (int32_t)sy},
                         .extent = { sw, sh } };
        vkCmdSetViewport(cb, 0, 1, &dvp);
        vkCmdSetScissor(cb, 0, 1, &dsc);
    }

    /* Stage 85b: bind the descriptor set for translated resource-using
     * pipelines (set 0 with the guest's buffers). NULL for the substitute path. */
    if (desc_set != VK_NULL_HANDLE && pipe_layout != VK_NULL_HANDLE) {
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipe_layout, 0, 1, &desc_set, 0, NULL);
    }

    /* Vertex-input path: bind the guest's vertex buffer at binding 0 so the
     * pipeline's vertex attributes are fed real positions. The pipeline was
     * built with a matching non-empty vertex-input state (pipeline_build). */
    if (vertex_buffer != VK_NULL_HANDLE) {
        VkDeviceSize voff = 0;
        vkCmdBindVertexBuffers(cb, 0, 1, &vertex_buffer, &voff);
    }

    /* Record draw command */
    if (indexed) {
        /* Indexed draw — vertex_count holds index_count per compute_inner_ops.c conventions */
        LAGFX_LOG("draw_record_and_submit: indexed draw indices=%u instances=%u baseVertex=%d",
                  vertex_count, instance_count, first_vertex);
        vkCmdDrawIndexed(cb, vertex_count, instance_count, 0, first_vertex, first_instance);
    } else {
        /* Unindexed draw */
        LAGFX_LOG("draw_record_and_submit: unindexed draw vertices=%u instances=%u baseVertex=%d",
                  vertex_count, instance_count, first_vertex);
        vkCmdDraw(cb, vertex_count, instance_count, first_vertex, first_instance);
    }
    
    /* End rendering */
    vkCmdEndRendering(cb);
    
    /* End command buffer (pattern from triangle-lavapipe-e2e.c:503) */
    vr = vkEndCommandBuffer(cb);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("draw_record_and_submit: vkEndCommandBuffer failed (%d)", (int)vr);
        lagfx_vk_cmdbuf_free(vk, cb);
        return LAGFX_ERR_BACKEND;
    }
    
    /* Submit to queue with fence */
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    vr = vkCreateFence(vk->device, &fci, NULL, &fence);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("draw_record_and_submit: vkCreateFence failed (%d)", (int)vr);
        lagfx_vk_cmdbuf_free(vk, cb);
        return LAGFX_ERR_BACKEND;
    }
    
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb,
    };
    /* M3/perf: time the submit→fence-complete window — the per-draw
     * synchronous GPU round-trip (B3). At N draws/frame this is the prime
     * throughput suspect; aggregated per frame at readback. */
    uint64_t perf_t0 = getenv("LAGFX_PERF") ? lagfx_now_ns() : 0u;
    vr = vkQueueSubmit(vk->graphics_queue, 1, &si, fence);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("draw_record_and_submit: vkQueueSubmit failed (%d)", (int)vr);
        vkDestroyFence(vk->device, fence, NULL);
        lagfx_vk_cmdbuf_free(vk, cb);
        return LAGFX_ERR_BACKEND;
    }
    
    /* Wait for completion. 30 s (was 1 s): with real rasterization the
     * big CA fixed-function draws legitimately shade for seconds on
     * lavapipe; at 1 s the common case became timeout → ERR + unbounded
     * vkDeviceWaitIdle, serializing the drain for minutes and stalling
     * the guest's GPU stack (KICKOFF-shading-throughput). Env-tunable:
     * LAGFX_DRAW_FENCE_MS. */
    uint64_t timeout_ns = 30ull * 1000ull * 1000ull * 1000ull;
    {
        const char *fm = getenv("LAGFX_DRAW_FENCE_MS");
        if (fm && fm[0]) {
            unsigned long ms = strtoul(fm, NULL, 10);
            if (ms) timeout_ns = (uint64_t)ms * 1000000ull;
        }
    }
    uint64_t fence_t0 = lagfx_now_ns();
    vr = vkWaitForFences(vk->device, 1, &fence, VK_TRUE, timeout_ns);
    {
        uint64_t waited_ms = (lagfx_now_ns() - fence_t0) / 1000000ull;
        vk->last_draw_wait_ms = (uint32_t)waited_ms;
        if (waited_ms > 1000u)
            LAGFX_LOG("draw_record_and_submit: SLOW draw %llums "
                      "(indexed=%d count=%u)",
                      (unsigned long long)waited_ms, indexed ? 1 : 0,
                      vertex_count);
    }
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("draw_record_and_submit: vkWaitForFences failed/timeout (%d)", (int)vr);
        /* The submission is STILL RUNNING on the lavapipe worker —
         * destroying the fence + freeing the command buffer here was a
         * use-after-free (SIGSEGV inside libvulkan_lvp at the next
         * present; live-observed when a long-running dispatched shader
         * exceeded the fence timeout). Block until the device is idle —
         * bounded now that dispatched control flow carries a hard
         * iteration cap — then clean up safely. */
        vkDeviceWaitIdle(vk->device);
        vkDestroyFence(vk->device, fence, NULL);
        lagfx_vk_cmdbuf_free(vk, cb);
        return LAGFX_ERR_BACKEND;
    }
    
    /* Cleanup */
    vkDestroyFence(vk->device, fence, NULL);
    lagfx_vk_cmdbuf_free(vk, cb);

    if (perf_t0) {
        vk->perf_frame_draw_ns += lagfx_now_ns() - perf_t0;
        vk->perf_frame_draws   += 1u;
    }

    LAGFX_LOG("draw_record_and_submit: draw completed successfully (indexed=%d count=%u)",
              indexed ? 1 : 0, vertex_count);
    return LAGFX_OK;
}

#endif /* LAGFX_HAVE_VULKAN */
/* No vulkan-disabled stub — the function declaration is also gated in
 * draw_record.h, so callers only see it when LAGFX_HAVE_VULKAN is on. */

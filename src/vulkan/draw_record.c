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
        index_buffer_ref, VK_NULL_HANDLE);
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
    VkBuffer vertex_buffer) {

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
    
    /* Wait for completion (1 second timeout from triangle-lavapipe-e2e.c:510-513) */
    const uint64_t timeout_ns = 1ull * 1000ull * 1000ull * 1000ull;
    vr = vkWaitForFences(vk->device, 1, &fence, VK_TRUE, timeout_ns);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("draw_record_and_submit: vkWaitForFences failed/timeout (%d)", (int)vr);
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

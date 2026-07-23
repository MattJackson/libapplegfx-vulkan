/*
 * libapplegfx-vulkan — Vulkan render target + clear + readback (Phase 2.B)
 * src/vulkan/render_target.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Implements the first-pixel render context described in
 * mos/paravirt-re/phase-2-first-pixel-plan.md §2.A (render target) and
 * §2.B (guest → host frame path — the clear + readback). Callers live
 * in src/protocol/ops_display.c (triggers the clear when a guest
 * transaction lands) and src/display.c (pulls the pixels out when the
 * shell calls lagfx_display_read_frame).
 *
 * Design choices:
 *   - Dynamic rendering is the target path (VK_KHR_dynamic_rendering /
 *     Vulkan 1.3 core). Lavapipe on Mesa 24+ supports it; the wider
 *     libapplegfx-vulkan asks for it in lagfx_vk_init's feature chain
 *     (vk->have_dynamic_rendering is cached there and asserted here).
 *   - The image is allocated as COLOR_ATTACHMENT + TRANSFER_SRC; no
 *     TRANSFER_DST because we never upload into it on the Phase 2 path
 *     (the clear happens on-GPU via the render-pass load-op).
 *   - The readback staging buffer is HOST_VISIBLE + HOST_COHERENT so we
 *     can memcpy out without a flush. It is allocated per-readback and
 *     freed before returning; pooling is a Phase 5 optimisation.
 *   - Layout tracking lives on the render target itself (rt->layout),
 *     updated by the helpers so callers don't reason about it. On
 *     first clear the transition is from UNDEFINED; on repeat clears
 *     the transition is from TRANSFER_SRC_OPTIMAL (the state after a
 *     previous readback). Both cases are expressed as a single
 *     vkCmdPipelineBarrier inside lagfx_vk_render_clear_color.
 *
 * Failure-mode policy:
 *   - Allocation / layout / submit failures all return LAGFX_ERR_BACKEND
 *     with a clear log line; callers (display.c, ops_display.c) treat
 *     that as "no new frame" and the shell keeps its previous surface.
 *
 * Concurrency:
 *   - Not thread-safe. The library is single-threaded per device in
 *     Phase 2; render target methods assume exclusive access. This
 *     matches the serialised QEMU BQL that drives the wire path.
 */

#include "render_target.h"
#include "instance.h"
#include "command.h"
#include "common/log.h"
#include "common/perf.h"
#include <stdlib.h>

#include <stdlib.h>
#include <string.h>

#ifdef LAGFX_HAVE_VULKAN

/* --- Memory helpers -------------------------------------------- */

/* Pick a memory type index that satisfies (typeBits) and has all
 * required property flags set. Returns UINT32_MAX on miss. */
static uint32_t find_memory_type(VkPhysicalDevice phys,
                                 uint32_t typeBits,
                                 VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) == 0) {
            continue;
        }
        if ((mp.memoryTypes[i].propertyFlags & want) == want) {
            return i;
        }
    }
    return UINT32_MAX;
}

/* --- Render target create/destroy ------------------------------ */

lagfx_status_t lagfx_vk_render_target_create(struct lagfx_vk_state *vk,
                                             uint32_t width,
                                             uint32_t height,
                                             VkFormat format,
                                             lagfx_vk_render_target_t *out) {
    if (!vk || !out || width == 0u || height == 0u) {
        return LAGFX_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    if (!vk->initialized || vk->device == VK_NULL_HANDLE) {
        LAGFX_ERR("render_target_create: vk state not initialized");
        return LAGFX_ERR_BACKEND;
    }

    /* === Image + Memory ============================================ */
    /* Attempt 0: OPTIMAL tiling + DEVICE_LOCAL (real GPUs).
     * Attempt 1: LINEAR tiling + HOST_VISIBLE|HOST_COHERENT (lavapipe).
     *
     * Lavapipe typically exposes one memory type: HOST_VISIBLE +
     * HOST_COHERENT (no DEVICE_LOCAL).  An OPTIMAL-tiled image may fail
     * to bind with VK_ERROR_OUT_OF_DEVICE_MEMORY because lavapipe
     * cannot satisfy the internal resource for that tiling with
     * host-visible memory.  LINEAR tiling works universally on software
     * rasterisers. */
    struct {
        VkImageTiling         tiling;
        VkMemoryPropertyFlags mem_pref;
    } const rt_cfg[] = {
        { VK_IMAGE_TILING_OPTIMAL, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT },
        { VK_IMAGE_TILING_LINEAR,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
          | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT },
    };
    VkResult vr = VK_SUCCESS;
    bool bound = false;
    for (int attempt = 0; attempt < 2; ++attempt) {
        VkImageCreateInfo ici = {
            .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType     = VK_IMAGE_TYPE_2D,
            .format        = format,
            .extent        = { width, height, 1u },
            .mipLevels     = 1u,
            .arrayLayers   = 1u,
            .samples       = VK_SAMPLE_COUNT_1_BIT,
            .tiling        = rt_cfg[attempt].tiling,
            .usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                           | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                           | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        vr = vkCreateImage(vk->device, &ici, NULL, &out->image);
        if (vr != VK_SUCCESS) {
            LAGFX_WARN("render_target_create: attempt %d vkCreateImage "
                       "failed (tiling=%d VkResult=%d)",
                       attempt, (int)rt_cfg[attempt].tiling, (int)vr);
            continue;
        }

        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(vk->device, out->image, &req);

        uint32_t mtype = find_memory_type(vk->phys_device,
                                          req.memoryTypeBits,
                                          rt_cfg[attempt].mem_pref);
        if (mtype == UINT32_MAX) {
            mtype = find_memory_type(vk->phys_device,
                                     req.memoryTypeBits, 0u);
        }
        if (mtype == UINT32_MAX) {
            LAGFX_WARN("render_target_create: attempt %d no memory type "
                       "(typeBits=0x%x)", attempt, req.memoryTypeBits);
            vkDestroyImage(vk->device, out->image, NULL);
            out->image = VK_NULL_HANDLE;
            continue;
        }

        VkMemoryAllocateInfo mai = {
            .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize  = req.size,
            .memoryTypeIndex = mtype,
        };
        vr = vkAllocateMemory(vk->device, &mai, NULL, &out->memory);
        if (vr != VK_SUCCESS) {
            LAGFX_WARN("render_target_create: attempt %d vkAllocateMemory "
                       "failed (VkResult=%d)", attempt, (int)vr);
            vkDestroyImage(vk->device, out->image, NULL);
            out->image = VK_NULL_HANDLE;
            continue;
        }

        vr = vkBindImageMemory(vk->device, out->image, out->memory, 0);
        if (vr != VK_SUCCESS) {
            LAGFX_WARN("render_target_create: attempt %d vkBindImageMemory "
                       "failed (tiling=%d VkResult=%d)",
                       attempt, (int)rt_cfg[attempt].tiling, (int)vr);
            vkFreeMemory(vk->device, out->memory, NULL);
            vkDestroyImage(vk->device, out->image, NULL);
            out->image   = VK_NULL_HANDLE;
            out->memory  = VK_NULL_HANDLE;
            continue;
        }

        LAGFX_LOG("render_target_create: bound on attempt %d "
                  "(tiling=%d mtype=%u size=%zu)",
                  attempt, (int)rt_cfg[attempt].tiling, mtype,
                  (size_t)req.size);
        bound = true;
        break;
    }

    if (!bound) {
        LAGFX_ERR("render_target_create: all image+memory configurations "
                  "failed");
        return LAGFX_ERR_BACKEND;
    }

    /* === View ==================================================== */
    VkImageViewCreateInfo vci = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image    = out->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = format,
        .subresourceRange = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0,
            .levelCount     = 1,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
    };
    vr = vkCreateImageView(vk->device, &vci, NULL, &out->view);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("render_target_create: vkCreateImageView failed (VkResult=%d)",
                  (int)vr);
        vkFreeMemory(vk->device, out->memory, NULL);
        vkDestroyImage(vk->device, out->image, NULL);
        out->image = VK_NULL_HANDLE;
        out->memory = VK_NULL_HANDLE;
        return LAGFX_ERR_BACKEND;
    }

    out->width  = width;
    out->height = height;
    out->format = format;
    out->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    out->owns_resources = true;

    LAGFX_LOG("render_target_create: %ux%u fmt=%d image=%p view=%p mem=%p",
              width, height, (int)format,
              (void *)out->image, (void *)out->view, (void *)out->memory);
    return LAGFX_OK;
}

lagfx_status_t lagfx_vk_render_target_wrap(
    VkImage image, VkImageView view, VkDeviceMemory memory,
    uint32_t width, uint32_t height, VkFormat format,
    lagfx_vk_render_target_t *out)
{
    if (!out || image == VK_NULL_HANDLE || view == VK_NULL_HANDLE) {
        return LAGFX_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->image  = image;
    out->view   = view;
    out->memory = memory;
    out->width  = width;
    out->height = height;
    out->format = format;
    out->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    out->owns_resources = false;
    return LAGFX_OK;
}

void lagfx_vk_render_target_destroy(struct lagfx_vk_state *vk,
                                    lagfx_vk_render_target_t *rt) {
    if (!vk || !rt) {
        return;
    }
    if (!rt->owns_resources) {
        memset(rt, 0, sizeof(*rt));
        return;
    }
    if (vk->device == VK_NULL_HANDLE) {
        memset(rt, 0, sizeof(*rt));
        return;
    }
    if (rt->view != VK_NULL_HANDLE) {
        vkDestroyImageView(vk->device, rt->view, NULL);
    }
    if (rt->image != VK_NULL_HANDLE) {
        vkDestroyImage(vk->device, rt->image, NULL);
    }
    if (rt->memory != VK_NULL_HANDLE) {
        vkFreeMemory(vk->device, rt->memory, NULL);
    }
    memset(rt, 0, sizeof(*rt));
}

/* --- Layout transition helper ---------------------------------- */

static void pipeline_barrier(VkCommandBuffer cmd,
                             VkImage image,
                             VkImageLayout old_layout,
                             VkImageLayout new_layout,
                             VkAccessFlags src_access,
                             VkAccessFlags dst_access,
                             VkPipelineStageFlags src_stage,
                             VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask       = src_access,
        .dstAccessMask       = dst_access,
        .oldLayout           = old_layout,
        .newLayout           = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = image,
        .subresourceRange    = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0,
            .levelCount     = 1,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
    };
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0,
                         0, NULL,
                         0, NULL,
                         1, &b);
}

/* --- Clear color via dynamic rendering ------------------------- */

lagfx_status_t lagfx_vk_render_clear_color(struct lagfx_vk_state *vk,
                                           VkCommandBuffer cmd,
                                           lagfx_vk_render_target_t *rt,
                                           const float rgba[4]) {
    if (!vk || !rt || !rgba || cmd == VK_NULL_HANDLE) {
        return LAGFX_ERR_INVALID_ARG;
    }
    if (!vk->have_dynamic_rendering) {
        LAGFX_ERR("render_clear_color: dynamic_rendering unavailable — "
                  "Phase 2 requires it");
        return LAGFX_ERR_BACKEND;
    }
    if (rt->image == VK_NULL_HANDLE || rt->view == VK_NULL_HANDLE) {
        LAGFX_ERR("render_clear_color: render target not initialized");
        return LAGFX_ERR_INVALID_ARG;
    }

    /* Transition to COLOR_ATTACHMENT_OPTIMAL. Sources:
     *   UNDEFINED              → first use after create.
     *   TRANSFER_SRC_OPTIMAL   → after a previous readback.
     * Both pathways use the same destination layout; the access mask
     * flips between 0 and TRANSFER_READ based on prior layout. */
    VkAccessFlags src_access = 0;
    VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    if (rt->layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        src_access = VK_ACCESS_TRANSFER_READ_BIT;
        src_stage  = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    pipeline_barrier(cmd, rt->image,
                     rt->layout,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     src_access,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                     src_stage,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    rt->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    /* Begin dynamic rendering with one color attachment + clear-load. */
    VkRenderingAttachmentInfo color_att = {
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = rt->view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue  = { .color = { .float32 = { rgba[0], rgba[1],
                                                 rgba[2], rgba[3] } } },
    };
    VkRenderingInfo ri = {
        .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea           = { { 0, 0 }, { rt->width, rt->height } },
        .layerCount           = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments    = &color_att,
    };
    vkCmdBeginRendering(cmd, &ri);
    vkCmdEndRendering(cmd);

    LAGFX_LOG("render_clear_color: %ux%u clear=(%.3f,%.3f,%.3f,%.3f)",
              rt->width, rt->height,
              (double)rgba[0], (double)rgba[1],
              (double)rgba[2], (double)rgba[3]);
    return LAGFX_OK;
}

/* --- Readback --------------------------------------------------- */

lagfx_status_t lagfx_vk_render_target_readback(struct lagfx_vk_state *vk,
                                               lagfx_vk_render_target_t *rt,
                                               void *dst,
                                               size_t dst_size,
                                               size_t *out_stride) {
    if (!vk || !rt || !dst) {
        return LAGFX_ERR_INVALID_ARG;
    }
    if (!vk->initialized || vk->device == VK_NULL_HANDLE
        || vk->graphics_queue == VK_NULL_HANDLE
        || vk->cmd_pool == VK_NULL_HANDLE) {
        LAGFX_ERR("render_target_readback: vk state not initialized");
        return LAGFX_ERR_BACKEND;
    }
    if (rt->image == VK_NULL_HANDLE) {
        LAGFX_ERR("render_target_readback: rt not initialized");
        return LAGFX_ERR_INVALID_ARG;
    }

    /* vkCmdCopyImageToBuffer writes the image's REAL texel size — sizing the
     * staging buffer as 4 Bpp for an RGBA16F (8 Bpp) image overflows it by
     * w*h*4 bytes (lavapipe runs in-process: heap corruption killed two
     * containers minutes after a `passes` dump of the 66 MB backdrop). */
    size_t bpp = 4u;
    switch (rt->format) {
    case VK_FORMAT_R16G16B16A16_SFLOAT: bpp = 8u; break;
    case VK_FORMAT_R8_UNORM:            bpp = 1u; break;
    default:                            bpp = 4u; break;
    }
    const size_t stride = (size_t)rt->width * bpp;
    const size_t need   = stride * (size_t)rt->height;
    if (dst_size < need) {
        LAGFX_ERR("render_target_readback: dst_size=%zu < need=%zu",
                  dst_size, need);
        return LAGFX_ERR_INVALID_ARG;
    }
    if (out_stride) {
        *out_stride = stride;
    }

    /* === Staging buffer (host-visible + coherent) ================ */
    VkBuffer         staging_buf  = VK_NULL_HANDLE;
    VkDeviceMemory   staging_mem  = VK_NULL_HANDLE;
    VkCommandBuffer  cb           = VK_NULL_HANDLE;
    VkFence          fence        = VK_NULL_HANDLE;
    lagfx_status_t   result       = LAGFX_OK;

    /* M3/perf: readback is once per delivered frame, so it is the natural
     * frame boundary. Time the readback itself + report the frame interval
     * (→ FPS) and the per-frame draw-submit total accumulated in draw_record. */
    uint64_t perf_rb_t0 = getenv("LAGFX_PERF") ? lagfx_now_ns() : 0u;

    VkBufferCreateInfo bci = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = need,
        .usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkResult vr = vkCreateBuffer(vk->device, &bci, NULL, &staging_buf);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("render_target_readback: vkCreateBuffer failed (%d)", (int)vr);
        return LAGFX_ERR_BACKEND;
    }

    VkMemoryRequirements breq;
    vkGetBufferMemoryRequirements(vk->device, staging_buf, &breq);
    uint32_t mtype = find_memory_type(vk->phys_device, breq.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                      | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mtype == UINT32_MAX) {
        LAGFX_ERR("render_target_readback: no HOST_VISIBLE+COHERENT memory type");
        result = LAGFX_ERR_BACKEND;
        goto cleanup;
    }
    VkMemoryAllocateInfo mai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = breq.size,
        .memoryTypeIndex = mtype,
    };
    vr = vkAllocateMemory(vk->device, &mai, NULL, &staging_mem);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("render_target_readback: vkAllocateMemory(staging) failed (%d)",
                  (int)vr);
        result = LAGFX_ERR_BACKEND;
        goto cleanup;
    }
    vr = vkBindBufferMemory(vk->device, staging_buf, staging_mem, 0);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("render_target_readback: vkBindBufferMemory failed (%d)",
                  (int)vr);
        result = LAGFX_ERR_BACKEND;
        goto cleanup;
    }

    /* === Command buffer: transition + copy ======================= */
    lagfx_status_t cb_st = lagfx_vk_cmdbuf_alloc(vk, &cb);
    if (cb_st != LAGFX_OK) {
        result = cb_st;
        goto cleanup;
    }

    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vr = vkBeginCommandBuffer(cb, &bi);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("render_target_readback: vkBeginCommandBuffer failed (%d)",
                  (int)vr);
        result = LAGFX_ERR_BACKEND;
        goto cleanup;
    }

    /* LIVE RT CLEAR (GOAL-M2y diagnostic): passes LOAD the RT, so pixels
     * painted once during the boot burst fossilize — skip-based bisection
     * probes can't remove them. Touch /tmp/lagfx_clear.txt (docker exec) to
     * clear the RT to black ONCE in this readback's command buffer; the flag
     * file is consumed. Combined with /tmp/lagfx_skip.txt + a guest
     * recomposite this gives a clean-slate attribution probe. */
    bool do_clear = false;
    {
        FILE *cf = fopen("/tmp/lagfx_clear.txt", "r");
        if (cf) {
            fclose(cf);
            remove("/tmp/lagfx_clear.txt");
            do_clear = true;
            LAGFX_LOG("render_target_readback: LIVE CLEAR consumed — RT wiped to black");
        }
    }
    if (do_clear) {
        pipeline_barrier(cb, rt->image,
                         rt->layout,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue black = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } };
        VkImageSubresourceRange srr = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1, .layerCount = 1,
        };
        vkCmdClearColorImage(cb, rt->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &black, 1, &srr);
        pipeline_barrier(cb, rt->image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_ACCESS_TRANSFER_READ_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT);
    } else {
        pipeline_barrier(cb, rt->image,
                         rt->layout,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_ACCESS_TRANSFER_READ_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT);
    }

    VkBufferImageCopy region = {
        .bufferOffset      = 0,
        .bufferRowLength   = 0,   /* tightly packed */
        .bufferImageHeight = 0,
        .imageSubresource  = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel       = 0,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
        .imageOffset = { 0, 0, 0 },
        .imageExtent = { rt->width, rt->height, 1u },
    };
    vkCmdCopyImageToBuffer(cb, rt->image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           staging_buf, 1, &region);

    vr = vkEndCommandBuffer(cb);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("render_target_readback: vkEndCommandBuffer failed (%d)",
                  (int)vr);
        result = LAGFX_ERR_BACKEND;
        goto cleanup;
    }

    /* === Submit + fence-wait ===================================== */
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    vr = vkCreateFence(vk->device, &fci, NULL, &fence);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("render_target_readback: vkCreateFence failed (%d)", (int)vr);
        result = LAGFX_ERR_BACKEND;
        goto cleanup;
    }
    VkSubmitInfo si = {
        .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers    = &cb,
    };
    vr = vkQueueSubmit(vk->graphics_queue, 1, &si, fence);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("render_target_readback: vkQueueSubmit failed (%d)", (int)vr);
        result = LAGFX_ERR_BACKEND;
        goto cleanup;
    }
    /* 1 second timeout — lavapipe readback of ~8 MB is sub-ms; generous
     * for CI headroom. */
    const uint64_t timeout_ns = 1ull * 1000ull * 1000ull * 1000ull;
    vr = vkWaitForFences(vk->device, 1, &fence, VK_TRUE, timeout_ns);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("render_target_readback: vkWaitForFences failed/timeout (%d)",
                  (int)vr);
        result = LAGFX_ERR_BACKEND;
        goto cleanup;
    }

    /* === Map + memcpy ============================================ */
    void *mapped = NULL;
    vr = vkMapMemory(vk->device, staging_mem, 0, need, 0, &mapped);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("render_target_readback: vkMapMemory failed (%d)", (int)vr);
        result = LAGFX_ERR_BACKEND;
        goto cleanup;
    }
    memcpy(dst, mapped, need);
    vkUnmapMemory(vk->device, staging_mem);

    /* DISPLAY-PATH VALIDATOR (LAGFX_TEST_BOXES): paint RED(tl)/GREEN(c)/BLUE(br)
     * boxes into EVERY readback's output — the true convergence point for all
     * displayed pixels (read_frame + ASMBLIT present both land here). If the
     * screendump shows them, the readback→display chain is proven and the bands
     * are RT content; if not, the display bypasses this readback entirely. */
    if (getenv("LAGFX_TEST_BOXES") && stride >= 4u) {
        uint8_t *fb = (uint8_t *)dst; uint32_t W = rt->width, H = rt->height;
        struct { uint32_t x0,y0,x1,y1; uint8_t r,g,b; } bx[3] = {
            {  50u,  50u, 450u, 250u, 255u,   0u,   0u },
            { 760u, 440u,1160u, 640u,   0u, 255u,   0u },
            {1450u, 830u,1850u,1030u,   0u,   0u, 255u } };
        for (int i = 0; i < 3; i++)
            for (uint32_t y = bx[i].y0; y < bx[i].y1 && y < H; y++) {
                uint8_t *row = fb + (size_t)y * stride;
                for (uint32_t x = bx[i].x0; x < bx[i].x1 && x < W; x++) {
                    uint8_t *px = row + (size_t)x * 4u;
                    px[0]=bx[i].b; px[1]=bx[i].g; px[2]=bx[i].r; px[3]=255u;
                }
            }
    }

    rt->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    LAGFX_LOG("render_target_readback: %ux%u stride=%zu bytes=%zu OK",
              rt->width, rt->height, stride, need);

cleanup:
    if (fence != VK_NULL_HANDLE) {
        vkDestroyFence(vk->device, fence, NULL);
    }
    if (cb != VK_NULL_HANDLE) {
        lagfx_vk_cmdbuf_free(vk, cb);
    }
    if (staging_mem != VK_NULL_HANDLE) {
        vkFreeMemory(vk->device, staging_mem, NULL);
    }
    if (staging_buf != VK_NULL_HANDLE) {
        vkDestroyBuffer(vk->device, staging_buf, NULL);
    }

    if (perf_rb_t0) {
        uint64_t now = lagfx_now_ns();
        uint64_t rb_ns = now - perf_rb_t0;
        uint64_t interval_ns = vk->perf_last_frame_ns ? (now - vk->perf_last_frame_ns) : 0u;
        vk->perf_last_frame_ns = now;
        double interval_ms = (double)interval_ns / 1.0e6;
        LAGFX_LOG("PERF frame: interval=%.2fms (%.1f fps) draws=%llu draw_submit_total=%.2fms "
                  "readback=%.2fms (%ux%u, %zuKB)",
                  interval_ms, interval_ms > 0.0 ? 1000.0 / interval_ms : 0.0,
                  (unsigned long long)vk->perf_frame_draws,
                  (double)vk->perf_frame_draw_ns / 1.0e6,
                  (double)rb_ns / 1.0e6,
                  rt->width, rt->height, need / 1024u);
        vk->perf_frame_draws = 0u;
        vk->perf_frame_draw_ns = 0u;
    }
    return result;
}

#endif /* LAGFX_HAVE_VULKAN */

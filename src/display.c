/*
 * libapplegfx-vulkan — display lifecycle (Phase 2.B first-pixel)
 * src/display.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Phase 2.B display path: on create, allocate a VkImage-backed render
 * target sized to the display's first advertised mode; on Phase 2.B
 * clear-colour triggers (routed from ops_display.c via
 * lagfx_display_submit_clear_color), render the clear into the target
 * and read it back into a latched buffer; on read_frame, hand the
 * latched pixels to the shell.
 *
 * Fallbacks:
 *   - No Vulkan (LAGFX_HAVE_VULKAN unset): lagfx_display_new still
 *     succeeds; rt_ready stays false; read_frame returns
 *     LAGFX_ERR_NO_FRAME. Darwin dev path works unchanged.
 *   - Vulkan present but init failed (device->vk->initialized false):
 *     identical to no-Vulkan at runtime.
 */

#include "device.h"
#include "display.h"
#include "protocol/ops_display.h"
#include "vulkan/instance.h"
#include "vulkan/command.h"
#include "vulkan/render_target.h"
#include "vulkan/cursor.h"
#include "common/log.h"

#include <stdlib.h>
#include <string.h>

#ifdef LAGFX_HAVE_VULKAN
#  include <vulkan/vulkan.h>
#endif

/* Default scanout geometry if the descriptor carries no modes. 1920x1080
 * is the Phase 2 first-pixel baseline per
 * mos/paravirt-re/phase-2-first-pixel-plan.md §2.D. */
#define LAGFX_DISPLAY_DEFAULT_W 1920u
#define LAGFX_DISPLAY_DEFAULT_H 1080u

static void set_err(char **errp_out, const char *msg) {
    if (!errp_out) {
        return;
    }
    size_t len = strlen(msg) + 1;
    char *buf = (char *)malloc(len);
    if (buf) {
        memcpy(buf, msg, len);
    }
    *errp_out = buf;
}

/* Cursor state accessors. State lives on lagfx_device_t (see device.h
 * "Cursor state" doc-block — single cursor shared across all attached
 * displays, matching macOS semantics). These accessors take the
 * device pointer rather than reading a file-static so multi-device
 * builds (test harness, future fan-out) don't get cross-talk. */
const lagfx_cursor_show_state_t *
lagfx_device_last_cursor_show(const lagfx_device_t *dev) {
    static const lagfx_cursor_show_state_t empty = {0};
    return dev ? &dev->cursor_show : &empty;
}

const lagfx_cursor_glyph_state_t *
lagfx_device_last_cursor_glyph(const lagfx_device_t *dev) {
    static const lagfx_cursor_glyph_state_t empty = {0};
    return dev ? &dev->cursor_glyph : &empty;
}

/* Phase 2.B render-target lifecycle helpers. Factored so the no-vulkan
 * build can stub them cleanly. */
#ifdef LAGFX_HAVE_VULKAN

static void notify_mode_changed(lagfx_display_t *display, uint32_t width, uint32_t height) {
    if (display->desc.callbacks.mode_changed) {
        display->desc.callbacks.mode_changed(
            display->desc.callbacks.opaque,
            width, height);
    }
}

static void display_rt_create(lagfx_display_t *disp) {
    if (!disp || !disp->device || !disp->device->vk
        || !disp->device->vk->initialized) {
        return;
    }
    struct lagfx_vk_state *vk = disp->device->vk;
    uint32_t w = LAGFX_DISPLAY_DEFAULT_W;
    uint32_t h = LAGFX_DISPLAY_DEFAULT_H;
    if (disp->desc.modes && disp->desc.mode_count > 0u
        && disp->desc.modes[0].width_px > 0u
        && disp->desc.modes[0].height_px > 0u
        && disp->desc.modes[0].width_px <= 4096u
        && disp->desc.modes[0].height_px <= 4096u) {
        w = disp->desc.modes[0].width_px;
        h = disp->desc.modes[0].height_px;
    }
    if (w == 0u || h == 0u) {
        w = LAGFX_DISPLAY_DEFAULT_W;
        h = LAGFX_DISPLAY_DEFAULT_H;
    }

    /* Publish dimensions before any notify_mode_changed call below.
     * Pass explicit (w, h) to the callback instead of reading from
     * internal state — this avoids the tight coupling that caused
     * regression e0ba3a5 → 20fe6c9 in libapplegfx-vulkan. */
    LAGFX_DISPLAY_RT_LOCK(disp);
    disp->rt_width = w;
    disp->rt_height = h;
    LAGFX_DISPLAY_RT_UNLOCK(disp);

    if (vk->frame_image != VK_NULL_HANDLE
        && vk->frame_image_w == w && vk->frame_image_h == h) {
        lagfx_status_t st = lagfx_vk_render_target_wrap(
            vk->frame_image, vk->frame_image_view,
            vk->frame_image_mem, w, h,
            VK_FORMAT_B8G8R8A8_UNORM, &disp->rt);
        if (st == LAGFX_OK) {
            LAGFX_LOG("display_rt_create: wrapped pipeline frame image "
                      "(%ux%u, no extra allocation)", w, h);
            disp->rt_ready = true;
            notify_mode_changed(disp, w, h);
            return;
        }
    }

    lagfx_status_t st = lagfx_vk_render_target_create(
        disp->device->vk, w, h, VK_FORMAT_B8G8R8A8_UNORM, &disp->rt);
    if (st != LAGFX_OK) {
        LAGFX_ERR("display_new: render_target_create failed (%d) — "
                  "read_frame will return NO_FRAME", (int)st);
        disp->rt_ready = false;
        notify_mode_changed(disp, w, h);
        return;
    }
    disp->rt_ready = true;
    notify_mode_changed(disp, w, h);
}

static void display_rt_destroy(lagfx_display_t *disp) {
    if (!disp || !disp->rt_ready) {
        return;
    }
    if (disp->device && disp->device->vk) {
        lagfx_vk_render_target_destroy(disp->device->vk, &disp->rt);
    }
    disp->rt_ready = false;
}
#else
static void display_rt_create(lagfx_display_t *disp) { (void)disp; }
static void display_rt_destroy(lagfx_display_t *disp) { (void)disp; }
#endif

static void notify_frame_ready(lagfx_display_t *display) {
    if (display->new_frame_ready
        && display->desc.callbacks.frame_ready) {
        display->desc.callbacks.frame_ready(
            display->desc.callbacks.opaque);
    }
}

static void set_frame_ready(lagfx_display_t *display) {
    display->new_frame_ready = true;
    notify_frame_ready(display);
}

lagfx_display_t *lagfx_display_new(lagfx_device_t *device,
                                    const lagfx_display_descriptor_t *desc,
                                    uint32_t port, uint32_t serial_num,
                                    char **errp_out) {
    if (!lagfx_device_is_valid(device)) {
        set_err(errp_out, "lagfx_display_new: invalid device");
        return NULL;
    }
    if (!desc) {
        set_err(errp_out, "lagfx_display_new: desc is NULL");
        return NULL;
    }

    lagfx_display_t *disp = (lagfx_display_t *)calloc(1, sizeof(*disp));
    if (!disp) {
        set_err(errp_out, "lagfx_display_new: out of memory");
        return NULL;
    }

    disp->magic      = LAGFX_DISPLAY_MAGIC;
    disp->device     = device;
    disp->desc       = *desc;  /* shallow; modes ptr not deep-copied */
    disp->port       = port;
    disp->serial_num = serial_num;
    disp->cursor_pos = (lagfx_coord_t){ 0, 0 };
    disp->has_frame  = 0;
    disp->new_frame_ready = false;

    /* Initialize thread-safety mutex for rt_* fields. */
    pthread_mutex_init(&disp->rt_lock, NULL);

    int rc = lagfx_device_attach_display(device, disp);
    if (rc != LAGFX_OK) {
        set_err(errp_out,
                "lagfx_display_new: device at max display count");
        free(disp);
        return NULL;
    }

    /* Phase 2.B: allocate the render target. Failure is non-fatal —
     * the display can still exist (read_frame will return NO_FRAME). */
    display_rt_create(disp);

    LAGFX_LOG("display_new: disp=%p dev=%p port=%u serial=%u name=%s "
              "rt_ready=%d %ux%u",
              (void *)disp, (void *)device, port, serial_num,
              desc->name ? desc->name : "(null)",
              (int)disp->rt_ready, disp->rt_width, disp->rt_height);

    return disp;
}

#ifdef LAGFX_HAVE_VULKAN
/* Forward decl — implementation lives near the persistent staging
 * ring helpers below lagfx_display_submit_rendered_frame. */
static void display_staging_slot_destroy(struct lagfx_vk_state *vk,
                                          lagfx_display_staging_slot_t *s);
#endif

void lagfx_display_free(lagfx_display_t *display) {
    if (!display) {
        return;
    }
    if (display->magic != LAGFX_DISPLAY_MAGIC) {
        LAGFX_ERR("display_free: bad magic on %p (got 0x%08x)",
                  (void *)display, display->magic);
        return;
    }

#ifdef LAGFX_HAVE_VULKAN
    /* Tear down the persistent staging ring before the Vulkan device
     * goes away. Walks all slots regardless of staging_size so partial
     * inits unwind cleanly. */
    if (display->device && display->device->vk
        && display->device->vk->initialized) {
        struct lagfx_vk_state *vk = display->device->vk;
        for (uint32_t i = 0; i < LAGFX_DISPLAY_STAGING_SLOTS; ++i) {
            lagfx_display_staging_slot_t *s = &display->staging[i];
            if (s->in_flight && s->fence != VK_NULL_HANDLE) {
                vkWaitForFences(vk->device, 1, &s->fence, VK_TRUE,
                                1000ull * 1000ull * 1000ull);
            }
            display_staging_slot_destroy(vk, s);
        }
        display->staging_size = 0;
    }
#endif

    display_rt_destroy(display);

    /* Destroy thread-safety mutex. */
    pthread_mutex_destroy(&display->rt_lock);

    /* Clean up fallback pixels buffer. */
    if (display->fallback_pixels) {
        free(display->fallback_pixels);
        display->fallback_pixels = NULL;
        display->fallback_stride = 0;
        display->fallback_bytes = 0;
    }

    if (display->device) {
        lagfx_device_detach_display(display->device, display);
    }

    LAGFX_LOG("display_free: disp=%p", (void *)display);

    memset(display, 0, sizeof(*display));
    free(display);
}

lagfx_coord_t lagfx_display_cursor_position(lagfx_display_t *display) {
    if (!display || display->magic != LAGFX_DISPLAY_MAGIC) {
        return (lagfx_coord_t){ 0, 0 };
    }
    return display->cursor_pos;
}

lagfx_status_t lagfx_display_read_frame(lagfx_display_t *display,
                                         void *dst,
                                         size_t dst_size_bytes,
                                         size_t *stride_out,
                                         bool *new_frame_out) {
    if (!display || display->magic != LAGFX_DISPLAY_MAGIC) {
        return LAGFX_ERR_INVALID_ARG;
    }

    /* Default outs — preserved on the NO_FRAME path so callers can
     * check uniformly. */
    if (new_frame_out) {
        *new_frame_out = false;
    }
    if (stride_out) {
        *stride_out = 0;
    }

    /* If no frame has rendered since the last read, report NO_FRAME.
     * This is the steady-state "nothing changed" path — shells that
     * poll via GraphicHwOps.gfx_update hit it most frames. */
    if (!display->new_frame_ready) {
        return LAGFX_ERR_NO_FRAME;
    }

#ifdef LAGFX_HAVE_VULKAN
    /* Fallback path: use pre-stored pixels from CmdDisplaySwapMapping
     * miss (no scanout buffer registered). This enables display output
     * even when macOS hasn't called IOFBSetDisplayModeAndDepth. */
    if (display->fallback_pixels && dst) {
        size_t copy_len = display->fallback_bytes;
        if (dst_size_bytes < copy_len) {
            copy_len = dst_size_bytes;
        }
        memcpy(dst, display->fallback_pixels, copy_len);
        if (stride_out) {
            *stride_out = display->fallback_stride;
        }
        if (new_frame_out) {
            *new_frame_out = true;
        }
        /* Consume fallback pixels — they've been delivered. */
        free(display->fallback_pixels);
        display->fallback_pixels = NULL;
        display->fallback_stride = 0;
        display->fallback_bytes = 0;
        display->new_frame_ready = false;
        display->has_frame = 1;
        return LAGFX_OK;
    }

    /* rt_* fields accessed under lock for thread safety. */
    bool ready;
    LAGFX_DISPLAY_RT_LOCK(display);
    ready = display->rt_ready;  /* Only need this for the check */
    LAGFX_DISPLAY_RT_UNLOCK(display);

    if (!ready || !display->device
        || !display->device->vk || !display->device->vk->initialized) {
        /* Flag says "frame ready" but no backend — unusual; clear
         * the flag so we don't loop. */
        display->new_frame_ready = false;
        return LAGFX_ERR_NO_FRAME;
    }
    if (!dst) {
        return LAGFX_ERR_INVALID_ARG;
    }

    size_t stride = 0;
    /* Use local copy of rt for readback — the render target itself is not
     * mutated, but we need to ensure we're reading consistent dimensions. */
    lagfx_status_t st = lagfx_vk_render_target_readback(
        display->device->vk, &display->rt, dst, dst_size_bytes, &stride);
    if (st != LAGFX_OK) {
        LAGFX_ERR("display_read_frame: readback failed (%d)", (int)st);
        /* Don't clear the flag — caller can retry; but if the failure
         * is persistent we don't want to busy-loop. For Phase 2 we
         * clear it; a repeat transaction will re-set. */
        display->new_frame_ready = false;
        return st;
    }

    if (stride_out) {
        *stride_out = stride;
    }
    if (new_frame_out) {
        *new_frame_out = true;
    }
    display->new_frame_ready = false;
    display->has_frame = 1;
    return LAGFX_OK;
#else
    /* No backend built — clear the flag, keep reporting NO_FRAME. */
    display->new_frame_ready = false;
    return LAGFX_ERR_NO_FRAME;
#endif
}

/* === Phase 2.B trigger hook ===================================
 *
 * Called by src/protocol/ops_display.c when a display transaction
 * carrying a clear-colour attachment lands. Records a one-shot
 * VkCommandBuffer with the clear, submits, fence-waits, and flips
 * the new_frame_ready flag so the next read_frame will extract
 * pixels via readback.
 *
 * On no-vulkan builds this just flips the flag; read_frame clears it
 * again with NO_FRAME. Same for vulkan-present-but-init-failed and
 * rt_ready=false.
 * ============================================================= */
lagfx_status_t lagfx_display_submit_clear_color(lagfx_display_t *display,
                                                const float rgba[4],
                                                uint64_t scanout_gpa,
                                                uint64_t scanout_length) {
    if (!display || display->magic != LAGFX_DISPLAY_MAGIC) {
        return LAGFX_ERR_INVALID_ARG;
    }

#ifdef LAGFX_HAVE_VULKAN
    if (!display->rt_ready || !display->device
        || !display->device->vk || !display->device->vk->initialized) {
        /* Flag the new-frame state so read_frame doesn't silently hide
         * the absence of a backend — read_frame will clear it with
         * NO_FRAME and the shell sees the signal. */
        set_frame_ready(display);
        (void)scanout_gpa;
        (void)scanout_length;
        return LAGFX_OK;
    }

    struct lagfx_vk_state *vk = display->device->vk;

    /* Snapshot rt size once under the lock — reused for cursor draw,
     * post-submit log, and DMA writeback below. Avoids racing with a
     * concurrent display_rt_create that could otherwise let the cmdbuf
     * encode one size and the writeback use another. */
    uint32_t rt_w, rt_h;
    LAGFX_DISPLAY_RT_LOCK(display);
    rt_w = display->rt_width;
    rt_h = display->rt_height;
    LAGFX_DISPLAY_RT_UNLOCK(display);

    float rgba_local[4];
    if (rgba) {
        rgba_local[0] = rgba[0];
        rgba_local[1] = rgba[1];
        rgba_local[2] = rgba[2];
        rgba_local[3] = rgba[3];
    } else {
        rgba_local[0] = rgba_local[1] = rgba_local[2] = 0.f;
        rgba_local[3] = 1.f;
    }

    const lagfx_cursor_show_state_t  *cs =
        lagfx_device_last_cursor_show(display->device);
    const lagfx_cursor_glyph_state_t *cg =
        lagfx_device_last_cursor_glyph(display->device);
    bool want_cursor = (cs && cs->visible && cg && cg->captured_len > 0);

    if (want_cursor && !vk->cursor_glyph_valid) {
        lagfx_status_t up_st = lagfx_vk_cursor_upload_glyph(
            vk, cg->bytes, cg->width, cg->height, cg->bytes_per_row);
        if (up_st != LAGFX_OK) {
            LAGFX_WARN("display_submit_clear: cursor glyph upload failed (%d)",
                       (int)up_st);
            want_cursor = false;
        }
    }

    VkCommandBuffer cb = VK_NULL_HANDLE;
    lagfx_status_t cb_st = lagfx_vk_cmdbuf_alloc(vk, &cb);
    if (cb_st != LAGFX_OK) {
        return cb_st;
    }

    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VkResult vr = vkBeginCommandBuffer(cb, &bi);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("display_submit_clear: vkBeginCommandBuffer failed (%d)",
                  (int)vr);
        lagfx_vk_cmdbuf_free(vk, cb);
        return LAGFX_ERR_BACKEND;
    }

    lagfx_vk_render_target_t *rt = &display->rt;

    VkAccessFlags src_access = 0;
    VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    if (rt->layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        src_access = VK_ACCESS_TRANSFER_READ_BIT;
        src_stage  = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    VkImageMemoryBarrier barrier = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask       = src_access,
        .dstAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout           = rt->layout,
        .newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = rt->image,
        .subresourceRange    = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0,
            .levelCount     = 1,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
    };
    vkCmdPipelineBarrier(cb, src_stage,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, NULL, 0, NULL, 1, &barrier);
    rt->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkRenderingAttachmentInfo color_att = {
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = rt->view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue  = { .color = { .float32 = { rgba_local[0], rgba_local[1],
                                                  rgba_local[2],
                                                  rgba_local[3] } } },
    };
    VkRenderingInfo ri = {
        .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea           = { { 0, 0 }, { rt->width, rt->height } },
        .layerCount           = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments    = &color_att,
    };
    vkCmdBeginRendering(cb, &ri);

    if (want_cursor) {
        lagfx_vk_cursor_draw(vk, cb, rt_w, rt_h,
                             cs->x, cs->y, cs->hot_x, cs->hot_y);
    }

    vkCmdEndRendering(cb);

    vr = vkEndCommandBuffer(cb);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("display_submit_clear: vkEndCommandBuffer failed (%d)",
                  (int)vr);
        lagfx_vk_cmdbuf_free(vk, cb);
        return LAGFX_ERR_BACKEND;
    }

    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    vr = vkCreateFence(vk->device, &fci, NULL, &fence);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("display_submit_clear: vkCreateFence failed (%d)", (int)vr);
        lagfx_vk_cmdbuf_free(vk, cb);
        return LAGFX_ERR_BACKEND;
    }

    VkSubmitInfo si = {
        .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers    = &cb,
    };
    vr = vkQueueSubmit(vk->graphics_queue, 1, &si, fence);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("display_submit_clear: vkQueueSubmit failed (%d)", (int)vr);
        vkDestroyFence(vk->device, fence, NULL);
        lagfx_vk_cmdbuf_free(vk, cb);
        return LAGFX_ERR_BACKEND;
    }

    const uint64_t timeout_ns = 1ull * 1000ull * 1000ull * 1000ull;
    vr = vkWaitForFences(vk->device, 1, &fence, VK_TRUE, timeout_ns);
    vkDestroyFence(vk->device, fence, NULL);
    lagfx_vk_cmdbuf_free(vk, cb);

    if (vr != VK_SUCCESS) {
        LAGFX_ERR("display_submit_clear: vkWaitForFences failed/timeout (%d)",
                  (int)vr);
        return LAGFX_ERR_BACKEND;
    }

    set_frame_ready(display);
    LAGFX_LOG("display_submit_clear: %ux%u clear=(%.3f,%.3f,%.3f,%.3f) OK",
              rt_w, rt_h,
              (double)rgba_local[0], (double)rgba_local[1],
              (double)rgba_local[2], (double)rgba_local[3]);

    /* M4 GAP #1: DMA the rendered pixels into the guest's scanout
     * buffer at the GPA captured by the last CmdDisplaySwapMapping.
     * Without this, the clear lands in host staging only and noVNC /
     * guest VM both see an unchanged framebuffer. The readback path
     * here is a second round-trip (staging buffer → host memcpy →
     * shell.write_memory → guest RAM); a future optimisation could
     * fold the copy into the same fence-waited cmdbuf. For Phase 2
     * first-pixel correctness, the extra hop is acceptable.
     *
     * Skipped when:
     *   - caller didn't carry a scanout GPA (scanout_gpa == 0),
     *   - shell didn't register a write_memory callback (older shells
     *     predating libapplegfx-vulkan@d3d7c79), or
     *   - length is zero / not large enough for one row.
     * Failure here is non-fatal — new_frame_ready already set, the
     * shell's read_frame path still works for local noVNC. */
    if (scanout_gpa != 0ull && scanout_length > 0ull
        && display->device->desc.shell.write_memory != NULL) {
        const uint32_t stride_expected = rt_w * 4u;
        const size_t rt_bytes = (size_t)rt_h * (size_t)stride_expected;
        if (scanout_length < rt_bytes) {
            LAGFX_WARN("display_submit_clear: scanout length %llu < "
                       "render target bytes %zu — skipping DMA writeback "
                       "(gpa=0x%llx)",
                       (unsigned long long)scanout_length, rt_bytes,
                       (unsigned long long)scanout_gpa);
        } else {
            uint8_t *pixels = (uint8_t *)malloc(rt_bytes);
            if (!pixels) {
                LAGFX_ERR("display_submit_clear: OOM allocating %zu-byte "
                          "readback buffer", rt_bytes);
            } else {
                size_t stride_actual = 0;
                lagfx_status_t rb_st = lagfx_vk_render_target_readback(
                    vk, &display->rt, pixels, rt_bytes, &stride_actual);
                if (rb_st != LAGFX_OK) {
                    LAGFX_ERR("display_submit_clear: readback for DMA "
                              "writeback failed (%d)", (int)rb_st);
                } else {
                    if (!display->device->desc.shell.write_memory(
                            display->device->desc.shell.opaque,
                            scanout_gpa, (uint64_t)rt_bytes, pixels)) {
                        LAGFX_WARN("display_submit_clear: DMA writeback to "
                                   "gpa=0x%llx (%zu bytes) failed",
                                   (unsigned long long)scanout_gpa,
                                   rt_bytes);
                    } else {
                        LAGFX_LOG("display_submit_clear: DMA writeback "
                                  "gpa=0x%llx bytes=%zu OK",
                                  (unsigned long long)scanout_gpa,
                                  rt_bytes);
                    }
                }
                free(pixels);
            }
        }
    }

    return LAGFX_OK;
#else
    (void)rgba;
    (void)scanout_gpa;
    (void)scanout_length;
    /* No backend — just flip the flag so semantics stay uniform from
     * the protocol decoder's perspective. read_frame will clear it. */
    set_frame_ready(display);
    return LAGFX_OK;
#endif
}

#ifdef LAGFX_HAVE_VULKAN
static uint32_t display_find_memory_type(VkPhysicalDevice phys,
                                          uint32_t typeBits,
                                          VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) == 0) continue;
        if ((mp.memoryTypes[i].propertyFlags & want) == want) return i;
    }
    return UINT32_MAX;
}

/* === Persistent staging-buffer ring ============================
 *
 * Each present (lagfx_display_submit_rendered_frame DMA path)
 * previously did vkCreateBuffer + vkAllocateMemory + vkBindBufferMemory
 * + vkCreateFence + submit + vkDestroyBuffer / vkFreeMemory /
 * vkDestroyFence. Repeated 30×/s, that's a real bottleneck through
 * Mesa's allocator.
 *
 * Two persistent slots, each carrying its own VkBuffer +
 * VkDeviceMemory (persistently mapped) + VkFence. Allocated on
 * first present; rebuilt if rt resizes. */
static void display_staging_slot_destroy(struct lagfx_vk_state *vk,
                                          lagfx_display_staging_slot_t *s) {
    if (!vk || !s) return;
    if (s->mapped && s->memory != VK_NULL_HANDLE) {
        vkUnmapMemory(vk->device, s->memory);
        s->mapped = NULL;
    }
    if (s->fence != VK_NULL_HANDLE) {
        vkDestroyFence(vk->device, s->fence, NULL);
        s->fence = VK_NULL_HANDLE;
    }
    if (s->buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(vk->device, s->buffer, NULL);
        s->buffer = VK_NULL_HANDLE;
    }
    if (s->memory != VK_NULL_HANDLE) {
        vkFreeMemory(vk->device, s->memory, NULL);
        s->memory = VK_NULL_HANDLE;
    }
    s->size = 0;
    s->in_flight = false;
}

static lagfx_status_t display_staging_slot_init(struct lagfx_vk_state *vk,
                                                 lagfx_display_staging_slot_t *s,
                                                 size_t bytes) {
    if (!vk || !s || bytes == 0) return LAGFX_ERR_INVALID_ARG;

    VkBufferCreateInfo bci = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = bytes,
        .usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkResult vr = vkCreateBuffer(vk->device, &bci, NULL, &s->buffer);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("staging_slot_init: vkCreateBuffer (%zu bytes) failed (%d)",
                  bytes, (int)vr);
        return LAGFX_ERR_BACKEND;
    }

    VkMemoryRequirements breq;
    vkGetBufferMemoryRequirements(vk->device, s->buffer, &breq);
    uint32_t mtype = display_find_memory_type(
        vk->phys_device, breq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
        | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mtype == UINT32_MAX) {
        LAGFX_ERR("staging_slot_init: no HOST_VISIBLE+COHERENT");
        display_staging_slot_destroy(vk, s);
        return LAGFX_ERR_BACKEND;
    }

    VkMemoryAllocateInfo mai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = breq.size,
        .memoryTypeIndex = mtype,
    };
    vr = vkAllocateMemory(vk->device, &mai, NULL, &s->memory);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("staging_slot_init: vkAllocateMemory failed (%d)", (int)vr);
        display_staging_slot_destroy(vk, s);
        return LAGFX_ERR_BACKEND;
    }
    vr = vkBindBufferMemory(vk->device, s->buffer, s->memory, 0);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("staging_slot_init: vkBindBufferMemory failed (%d)", (int)vr);
        display_staging_slot_destroy(vk, s);
        return LAGFX_ERR_BACKEND;
    }

    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    vr = vkCreateFence(vk->device, &fci, NULL, &s->fence);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("staging_slot_init: vkCreateFence failed (%d)", (int)vr);
        display_staging_slot_destroy(vk, s);
        return LAGFX_ERR_BACKEND;
    }

    /* Persistent map — HOST_COHERENT means we don't need explicit
     * flushes; reads always see fresh data once the fence completes. */
    vr = vkMapMemory(vk->device, s->memory, 0, breq.size, 0, &s->mapped);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("staging_slot_init: vkMapMemory failed (%d)", (int)vr);
        display_staging_slot_destroy(vk, s);
        return LAGFX_ERR_BACKEND;
    }

    s->size = bytes;
    s->in_flight = false;
    return LAGFX_OK;
}

/* Ensure the per-display staging ring has slots sized for `bytes`.
 * Rebuilds on size change. Caller must hold rt_lock or otherwise
 * serialise vs display_rt_create. */
static lagfx_status_t display_staging_ring_ensure(lagfx_display_t *display,
                                                   struct lagfx_vk_state *vk,
                                                   size_t bytes) {
    if (display->staging_size == bytes) return LAGFX_OK;

    /* Wait out any in-flight fences before tearing down. */
    for (uint32_t i = 0; i < LAGFX_DISPLAY_STAGING_SLOTS; ++i) {
        lagfx_display_staging_slot_t *s = &display->staging[i];
        if (s->in_flight && s->fence != VK_NULL_HANDLE) {
            vkWaitForFences(vk->device, 1, &s->fence, VK_TRUE,
                            1000ull * 1000ull * 1000ull);
            s->in_flight = false;
        }
        display_staging_slot_destroy(vk, s);
    }
    display->staging_size = 0;
    display->staging_slot_idx = 0;

    for (uint32_t i = 0; i < LAGFX_DISPLAY_STAGING_SLOTS; ++i) {
        lagfx_status_t st = display_staging_slot_init(
            vk, &display->staging[i], bytes);
        if (st != LAGFX_OK) {
            /* Unwind partial init. */
            for (uint32_t j = 0; j < i; ++j) {
                display_staging_slot_destroy(vk, &display->staging[j]);
            }
            return st;
        }
    }
    display->staging_size = bytes;
    LAGFX_LOG("display_staging_ring_ensure: armed %u slots × %zu bytes",
              LAGFX_DISPLAY_STAGING_SLOTS, bytes);
    return LAGFX_OK;
}
#endif

lagfx_status_t lagfx_display_submit_rendered_frame(
    lagfx_display_t *display,
    uint64_t scanout_gpa,
    uint64_t scanout_length) {
    if (!display || display->magic != LAGFX_DISPLAY_MAGIC) {
        return LAGFX_ERR_INVALID_ARG;
    }

#ifdef LAGFX_HAVE_VULKAN
    /* Fallback path when macOS hasn't registered a scanout buffer:
     * CmdDisplaySwapMapping (opcode 0x12) not received yet. Read back
     * the render target directly and let QEMU's frame_ready callback
     * expose it via noVNC. */
    if (scanout_gpa == 0 || scanout_length == 0) {
        if (!display->device || !display->device->vk
            || !display->device->vk->initialized) {
            return LAGFX_OK;
        }
        struct lagfx_vk_state *vk = display->device->vk;
        if (vk->frame_image == VK_NULL_HANDLE) {
            return LAGFX_OK;
        }

        const uint32_t stride_expected = vk->frame_image_w * 4u;
        const size_t frame_bytes = (size_t)vk->frame_image_h
                                 * (size_t)stride_expected;
        if (frame_bytes == 0) {
            return LAGFX_OK;
        }

        uint8_t *pixels = malloc(frame_bytes);
        if (!pixels) {
            LAGFX_ERR("display_submit_rendered_frame: OOM allocating %zu-byte "
                      "readback buffer (fallback path)", frame_bytes);
            return LAGFX_ERR_BACKEND;
        }

        size_t stride_actual = 0;
        lagfx_status_t rb_st = lagfx_vk_render_target_readback(
            vk, &display->rt, pixels, frame_bytes, &stride_actual);
        if (rb_st != LAGFX_OK) {
            LAGFX_ERR("display_submit_rendered_frame: readback failed (%d)",
                      (int)rb_st);
            free(pixels);
            return rb_st;
        }

        /* Expose pixels via shell.read_memory callback so QEMU can
         * copy them to the DisplaySurface. We use the display's rt_width/rt_height
         * as the destination dimensions. */
        if (display->device->desc.shell.read_memory != NULL) {
            /* For now: store pixels in a static buffer and signal frame_ready.
             * The shell's read_frame path will pull them on next poll. */
            if (display->fallback_pixels) {
                free(display->fallback_pixels);
            }
            display->fallback_pixels = pixels;
            display->fallback_stride = stride_actual;
            display->fallback_bytes = frame_bytes;
        } else {
            /* No read_memory callback — just keep pixels for now.
             * Future: store in ring buffer. */
            LAGFX_WARN("display_submit_rendered_frame: no shell.read_memory "
                       "callback available for fallback path");
            free(pixels);
        }

        set_frame_ready(display);
        return LAGFX_OK;
    }

    if (!display->device || !display->device->vk
        || !display->device->vk->initialized) {
        return LAGFX_OK;
    }
    struct lagfx_vk_state *vk = display->device->vk;
    if (vk->frame_image == VK_NULL_HANDLE) {
        return LAGFX_OK;
    }
    if (display->device->desc.shell.write_memory == NULL) {
        return LAGFX_OK;
    }

    const uint32_t stride_expected = vk->frame_image_w * 4u;
    const size_t frame_bytes = (size_t)vk->frame_image_h
                             * (size_t)stride_expected;
    if (frame_bytes == 0) {
        return LAGFX_OK;
    }
    if (scanout_length < frame_bytes) {
        LAGFX_WARN("display_submit_rendered_frame: scanout_length=%llu < "
                   "frame_bytes=%zu — skipping DMA writeback",
                   (unsigned long long)scanout_length, frame_bytes);
        return LAGFX_OK;
    }

    /* Persistent staging-buffer ring — allocate (or resize) on first
     * present at this size, then reuse across frames. Old behaviour
     * was vkCreateBuffer + vkAllocateMemory + vkBindBufferMemory +
     * vkCreateFence per frame; at 30 Hz Stage-30 target that's 30
     * large allocations / sec through Mesa's allocator. */
    LAGFX_DISPLAY_RT_LOCK(display);
    lagfx_status_t ring_st = display_staging_ring_ensure(display, vk, frame_bytes);
    LAGFX_DISPLAY_RT_UNLOCK(display);
    if (ring_st != LAGFX_OK) {
        return ring_st;
    }

    /* Round-robin pick — wait on the prior fence for this slot if
     * still in-flight so we don't corrupt the persistent mapping. */
    uint32_t slot_idx = display->staging_slot_idx;
    lagfx_display_staging_slot_t *slot = &display->staging[slot_idx];
    display->staging_slot_idx =
        (slot_idx + 1u) % LAGFX_DISPLAY_STAGING_SLOTS;

    if (slot->in_flight) {
        VkResult wr = vkWaitForFences(vk->device, 1, &slot->fence, VK_TRUE,
                                       1000ull * 1000ull * 1000ull);
        if (wr != VK_SUCCESS) {
            LAGFX_ERR("display_submit_rendered_frame: prior fence wait failed (%d)",
                      (int)wr);
            return LAGFX_ERR_BACKEND;
        }
        slot->in_flight = false;
    }
    VkResult vr = vkResetFences(vk->device, 1, &slot->fence);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("display_submit_rendered_frame: vkResetFences failed (%d)",
                  (int)vr);
        return LAGFX_ERR_BACKEND;
    }

    VkCommandBuffer  cb     = VK_NULL_HANDLE;
    lagfx_status_t   result = LAGFX_OK;

    lagfx_status_t cb_st = lagfx_vk_cmdbuf_alloc(vk, &cb);
    if (cb_st != LAGFX_OK) {
        return cb_st;
    }

    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vr = vkBeginCommandBuffer(cb, &bi);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("display_submit_rendered_frame: vkBeginCommandBuffer failed (%d)",
                  (int)vr);
        result = LAGFX_ERR_BACKEND;
        goto cleanup_rendered;
    }

    {
        VkImageMemoryBarrier barrier1 = {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = vk->frame_image,
            .subresourceRange    = {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel   = 0,
                .levelCount     = 1,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
        };
        vkCmdPipelineBarrier(cb,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, NULL, 0, NULL, 1, &barrier1);
    }

    {
        VkBufferImageCopy region = {
            .bufferOffset      = 0,
            .bufferRowLength   = 0,
            .bufferImageHeight = 0,
            .imageSubresource  = {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel       = 0,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
            .imageOffset = { 0, 0, 0 },
            .imageExtent = { vk->frame_image_w, vk->frame_image_h, 1u },
        };
        vkCmdCopyImageToBuffer(cb, vk->frame_image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               slot->buffer, 1, &region);
    }

    {
        VkImageMemoryBarrier barrier2 = {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = vk->frame_image,
            .subresourceRange    = {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel   = 0,
                .levelCount     = 1,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
        };
        vkCmdPipelineBarrier(cb,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             0, 0, NULL, 0, NULL, 1, &barrier2);
    }

    vr = vkEndCommandBuffer(cb);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("display_submit_rendered_frame: vkEndCommandBuffer failed (%d)",
                  (int)vr);
        result = LAGFX_ERR_BACKEND;
        goto cleanup_rendered;
    }

    {
        VkSubmitInfo si = {
            .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers    = &cb,
        };
        vr = vkQueueSubmit(vk->graphics_queue, 1, &si, slot->fence);
        if (vr != VK_SUCCESS) {
            LAGFX_ERR("display_submit_rendered_frame: vkQueueSubmit failed (%d)",
                      (int)vr);
            result = LAGFX_ERR_BACKEND;
            goto cleanup_rendered;
        }
        slot->in_flight = true;
    }

    {
        const uint64_t timeout_ns = 1ull * 1000ull * 1000ull * 1000ull;
        vr = vkWaitForFences(vk->device, 1, &slot->fence, VK_TRUE, timeout_ns);
        if (vr != VK_SUCCESS) {
            LAGFX_ERR("display_submit_rendered_frame: vkWaitForFences failed (%d)",
                      (int)vr);
            result = LAGFX_ERR_BACKEND;
            goto cleanup_rendered;
        }
        slot->in_flight = false;
    }

    set_frame_ready(display);

    /* Persistent map — HOST_COHERENT guarantees the bytes the GPU
     * wrote are visible without an explicit vkInvalidateMappedMemoryRanges. */
    if (!display->device->desc.shell.write_memory(
            display->device->desc.shell.opaque,
            scanout_gpa, (uint64_t)frame_bytes, slot->mapped)) {
        LAGFX_WARN("display_submit_rendered_frame: DMA writeback to "
                   "gpa=0x%llx (%zu bytes) failed",
                   (unsigned long long)scanout_gpa, frame_bytes);
    } else {
        LAGFX_LOG("display_submit_rendered_frame: DMA writeback "
                  "gpa=0x%llx bytes=%zu OK (slot %u, persistent staging)",
                  (unsigned long long)scanout_gpa, frame_bytes, slot_idx);
    }

cleanup_rendered:
    if (cb != VK_NULL_HANDLE) {
        lagfx_vk_cmdbuf_free(vk, cb);
    }
    return result;
#else
    (void)scanout_gpa;
    (void)scanout_length;
    set_frame_ready(display);
    return LAGFX_OK;
#endif
}

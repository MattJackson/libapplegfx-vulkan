/*
 * libapplegfx-vulkan — display lifecycle (Phase 2.B first-pixel)
 * src/display.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
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
#include "vulkan/instance.h"
#include "vulkan/command.h"
#include "vulkan/render_target.h"
#include "common/log.h"

#include <stdlib.h>
#include <string.h>

#ifdef LAGFX_HAVE_VULKAN
#  include <vulkan/vulkan.h>
#endif

/* Default scanout geometry if the descriptor carries no modes. 1920x1080
 * is the Phase 2 first-pixel baseline per
 * mos/the internal spec */
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

/* Phase 2.B render-target lifecycle helpers. Factored so the no-vulkan
 * build can stub them cleanly. */
#ifdef LAGFX_HAVE_VULKAN
static void display_rt_create(lagfx_display_t *disp) {
    if (!disp || !disp->device || !disp->device->vk
        || !disp->device->vk->initialized) {
        return;
    }
    uint32_t w = LAGFX_DISPLAY_DEFAULT_W;
    uint32_t h = LAGFX_DISPLAY_DEFAULT_H;
    if (disp->desc.modes && disp->desc.mode_count > 0u) {
        w = disp->desc.modes[0].width_px;
        h = disp->desc.modes[0].height_px;
    }
    if (w == 0u || h == 0u) {
        w = LAGFX_DISPLAY_DEFAULT_W;
        h = LAGFX_DISPLAY_DEFAULT_H;
    }
    lagfx_status_t st = lagfx_vk_render_target_create(
        disp->device->vk, w, h, VK_FORMAT_B8G8R8A8_UNORM, &disp->rt);
    if (st != LAGFX_OK) {
        LAGFX_ERR("display_new: render_target_create failed (%d) — "
                  "read_frame will return NO_FRAME", (int)st);
        disp->rt_ready = false;
        return;
    }
    disp->rt_ready = true;
    disp->rt_width = w;
    disp->rt_height = h;
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

void lagfx_display_free(lagfx_display_t *display) {
    if (!display) {
        return;
    }
    if (display->magic != LAGFX_DISPLAY_MAGIC) {
        LAGFX_ERR("display_free: bad magic on %p (got 0x%08x)",
                  (void *)display, display->magic);
        return;
    }

    display_rt_destroy(display);

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
    if (!display->rt_ready || !display->device
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
    (void)dst;
    (void)dst_size_bytes;
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
        display->new_frame_ready = true;
        (void)scanout_gpa;
        (void)scanout_length;
        return LAGFX_OK;
    }

    struct lagfx_vk_state *vk = display->device->vk;

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

    lagfx_status_t clear_st = lagfx_vk_render_clear_color(vk, cb,
                                                          &display->rt,
                                                          rgba_local);
    if (clear_st != LAGFX_OK) {
        vkEndCommandBuffer(cb);
        lagfx_vk_cmdbuf_free(vk, cb);
        return clear_st;
    }

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

    display->new_frame_ready = true;
    LAGFX_LOG("display_submit_clear: %ux%u clear=(%.3f,%.3f,%.3f,%.3f) OK",
              display->rt_width, display->rt_height,
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
        const uint32_t stride_expected = display->rt_width * 4u;
        const size_t rt_bytes = (size_t)display->rt_height
                              * (size_t)stride_expected;
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
    display->new_frame_ready = true;
    return LAGFX_OK;
#endif
}

/*
 * libapplegfx-vulkan — internal display state
 * src/display.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef LIBAPPLEGFX_DISPLAY_INTERNAL_H
#define LIBAPPLEGFX_DISPLAY_INTERNAL_H

#include "libapplegfx-vulkan.h"
#include "device.h"
#include "vulkan/render_target.h"

struct lagfx_display {
    uint32_t magic;                     /* LAGFX_DISPLAY_MAGIC */
    lagfx_device_t *device;             /* owning device (not owned) */
    lagfx_display_descriptor_t desc;    /* copied */
    uint32_t port;
    uint32_t serial_num;

    /* Current cursor position. Updated by Phase 1.A.2 MMIO handlers;
     * for now stays at (0,0). */
    lagfx_coord_t cursor_pos;

    /* Phase 2.B render target: a VkImage+view+memory quartet sized
     * width_px × height_px in BGRA8Unorm. Populated by lagfx_display_new
     * when Vulkan is initialised; torn down in lagfx_display_free. In
     * no-vulkan builds rt is a placeholder and rt_ready stays false. */
    lagfx_vk_render_target_t rt;
    bool rt_ready;
    uint32_t rt_width;
    uint32_t rt_height;

    /* Latched "a new frame has rendered" flag. Set by the protocol
     * decoder (ops_display.c) when a clear-colour transaction has been
     * submitted + waited-on. Consumed + cleared by
     * lagfx_display_read_frame so the shell only calls dpy_gfx_update
     * once per rendered frame. */
    bool new_frame_ready;

    /* Phase 1.A.1 legacy: true once a frame has rendered (retained for
     * observability; superseded by new_frame_ready semantically). */
    int has_frame;
};

/* Internal accessor: called by ops_display.c when a clear-colour
 * transaction lands. Renders the clear into the display's render
 * target, reads the pixels back, and sets new_frame_ready. On
 * no-vulkan builds this records the clear colour + flips the flag
 * but does no Vulkan work — read_frame returns NO_FRAME in that
 * configuration anyway.
 *
 * rgba may be NULL in which case a (0,0,0,1) black clear is used;
 * Phase 2 callers always pass the transaction's last_clear_rgba.
 *
 * scanout_gpa / scanout_length are optional. When non-zero (and the
 * device shell supplied a write_memory callback), the rendered pixels
 * are DMA'd back to the guest's scanout buffer at that GPA after the
 * clear fence-waits. This closes M4 GAP #1: CmdDisplayTransaction3's
 * rendered output previously stopped at the shell's staging buffer;
 * now it lands in the guest-visible scanout VA captured by the prior
 * CmdDisplaySwapMapping (ops_display.c:213ff). Pass (0, 0) to skip
 * the writeback (legacy / unit-test callers that only care about the
 * flag state). */
lagfx_status_t lagfx_display_submit_clear_color(lagfx_display_t *display,
                                                const float rgba[4],
                                                uint64_t scanout_gpa,
                                                uint64_t scanout_length);

#endif /* LIBAPPLEGFX_DISPLAY_INTERNAL_H */

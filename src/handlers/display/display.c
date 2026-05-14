/*
 * libapplegfx-vulkan — Display handlers (stub implementations)
 * src/handlers/display/display.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "../handlers.h"
#include "common/le.h"
#include "common/log.h"

lagfx_handler_status_t lagfx_display_ack(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }

    /* CmdDisplayAck payload (8 bytes):
     *   +0  u32 display_id
     *   +4  u32 frame_id (present sequence number) */
    if (!hdr->payload || hdr->payload_size < 8) {
        LAGFX_WARN("CmdDisplayAck: payload too small (%u)", (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint32_t display_id = lagfx_le32(hdr->payload + 0);
    uint32_t frame_id   = lagfx_le32(hdr->payload + 4);

    LAGFX_LOG("CmdDisplayAck: display_id=%u frame_id=%u stamp=0x%08x",
              display_id, frame_id, hdr->stamp);

    /* TODO: Match frame_id to pending transactions; advance vblank counter;
     *       notify guest via stamp completion that present is complete. */

    return LAGFX_HANDLER_OK;
}

lagfx_handler_status_t lagfx_display_cursor_glyph(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }

    /* CmdDisplayCursorGlyph payload (32 bytes):
     *   +0  u32 display_id
     *   +4  u32 width
     *   +8  u32 height
     *   +12 u32 bytes_per_row
     *   +16 u32 hot_x
     *   +20 u32 hot_y
     *   +24 u64 glyph_va (guest VA of ARGB8888 pixel data) */
    if (!hdr->payload || hdr->payload_size < 32) {
        LAGFX_WARN("CmdDisplayCursorGlyph: payload too small (%u)", (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint32_t display_id   = lagfx_le32(hdr->payload + 0);
    uint32_t width        = lagfx_le32(hdr->payload + 4);
    uint32_t height       = lagfx_le32(hdr->payload + 8);
    uint32_t bytes_per_row = lagfx_le32(hdr->payload + 12);
    uint32_t hot_x        = lagfx_le32(hdr->payload + 16);
    uint32_t hot_y        = lagfx_le32(hdr->payload + 20);
    uint64_t glyph_va     = lagfx_le64(hdr->payload + 24);

    LAGFX_LOG("CmdDisplayCursorGlyph: display_id=%u %ux%d bpr=%u hot=(%u,%u) va=0x%llx",
              display_id, width, height, bytes_per_row, hot_x, hot_y, (unsigned long long)glyph_va);

    /* TODO: Read glyph pixels from guest VA via shell.read_memory; upload to Vulkan cursor texture;
     *       update g_cursor_glyph state for lagfx_display_submit_clear_color. */

    return LAGFX_HANDLER_OK;
}

lagfx_handler_status_t lagfx_display_cursor_show(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }

    /* CmdDisplayCursorShow payload (16 bytes):
     *   +0  u32 display_id
     *   +4  int16_t x (signed screen coords)
     *   +6  int16_t y
     *   +8  u32 visible (nonzero = show cursor)
     *   +12 u16 hot_x
     *   +14 u16 hot_y */
    if (!hdr->payload || hdr->payload_size < 16) {
        LAGFX_WARN("CmdDisplayCursorShow: payload too small (%u)", (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint32_t display_id = lagfx_le32(hdr->payload + 0);
    int16_t x           = (int16_t)lagfx_le16(hdr->payload + 4);
    int16_t y           = (int16_t)lagfx_le16(hdr->payload + 6);
    uint32_t visible    = lagfx_le32(hdr->payload + 8);
    uint16_t hot_x      = lagfx_le16(hdr->payload + 12);
    uint16_t hot_y      = lagfx_le16(hdr->payload + 14);

    LAGFX_LOG("CmdDisplayCursorShow: display_id=%u pos=(%d,%d) visible=%u hot=(%u,%u)",
              display_id, x, y, visible, hot_x, hot_y);

    /* Update cursor state for lagfx_display_submit_clear_color. */
    extern const lagfx_cursor_show_state_t *lagfx_ops_display_last_cursor_show(void);
    (void)display_id; (void)x; (void)y; (void)visible; (void)hot_x; (void)hot_y;

    /* TODO: Call QEMU dpy_mouse_set to move cursor on noVNC display. */

    return LAGFX_HANDLER_OK;
}

/* ================================================================
 * VChan Display Opcodes (0x01, 0x02, 0x04, 0x06, 0x07)
 * Sent on display channels ch 5+ using compact opcode namespace
 * ================================================================ */

lagfx_handler_status_t lagfx_display_vchan_setup_shared_state(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {

    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    if (!hdr->payload || hdr->payload_size < 8) {
        LAGFX_WARN("vchan_setup_shared_state: payload too small (%u, need 8)", (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint32_t display_index = lagfx_le32(hdr->payload + 0);
    uint32_t ss_pfn        = lagfx_le32(hdr->payload + 4);

    LAGFX_LOG("vchan_setup_shared_state: display[%u] ss_pfn=0x%x stamp=0x%08x",
              display_index, ss_pfn, hdr->stamp);

    if (ss_pfn == 0u) {
        LAGFX_WARN("vchan_setup_shared_state: ss_pfn=0 for display[%u]", display_index);
        return LAGFX_HANDLER_ERR_STATE;
    }

    /* Store shared state GPA and mark as installed */
    uint64_t ss_gpa = ((uint64_t)ss_pfn << 12);
    if (display_index < 16 && p->dev != NULL) {
        lagfx_device_t *dev = (lagfx_device_t *)p->dev;
        dev->display_ss_gpa[display_index] = ss_gpa;
        dev->display_ss_installed |= (1u << display_index);
        LAGFX_LOG("vchan_setup_shared_state: installed display[%u] at gpa=0x%llx",
                  display_index, (unsigned long long)ss_gpa);
    }

    /* Write shared state page fields */
    if (!p->dev || !((lagfx_device_t *)p->dev)->desc.shell.write_memory) {
        return LAGFX_HANDLER_OK;
    }

    lagfx_device_t *dev = (lagfx_device_t *)p->dev;
    uint16_t port = (uint16_t)display_index;
    dev->desc.shell.write_memory(
        dev->desc.shell.opaque, ss_gpa + 0x12u, sizeof(port), &port);

    uint32_t resp_code = 0u;
    dev->desc.shell.write_memory(
        dev->desc.shell.opaque, ss_gpa + 0x1cu, sizeof(resp_code), &resp_code);

    uint32_t conn_id = 1u;
    dev->desc.shell.write_memory(
        dev->desc.shell.opaque, ss_gpa + 0x00u, sizeof(conn_id), &conn_id);

    /* CRITICAL: Write online event flag IMMEDIATELY */
    uint32_t enabled = 0xCu;
    dev->desc.shell.write_memory(
        dev->desc.shell.opaque, ss_gpa + 0x104u, sizeof(enabled), &enabled);
    LAGFX_LOG("vchan_setup_shared_state: wrote ss[+0x104]=0xC (online event) for display[%u]", display_index);

    /* Write mode info */
    static const char mode_name[] = "1920x1080";
    dev->desc.shell.write_memory(
        dev->desc.shell.opaque, ss_gpa + 0x04u, sizeof(mode_name), mode_name);

    uint16_t width  = 1920u;
    uint16_t height = 1080u;
    dev->desc.shell.write_memory(
        dev->desc.shell.opaque, ss_gpa + 0x14u, sizeof(width), &width);
    dev->desc.shell.write_memory(
        dev->desc.shell.opaque, ss_gpa + 0x16u, sizeof(height), &height);

    uint16_t cursor_max = 64u;
    dev->desc.shell.write_memory(
        dev->desc.shell.opaque, ss_gpa + 0x18u, sizeof(cursor_max), &cursor_max);
    dev->desc.shell.write_memory(
        dev->desc.shell.opaque, ss_gpa + 0x1au, sizeof(cursor_max), &cursor_max);

    uint32_t fb_len = 1920u * 1080u * 4u;
    dev->desc.shell.write_memory(
        dev->desc.shell.opaque, ss_gpa + 0x20u, sizeof(fb_len), &fb_len);

    /* Publish a single pixel-format record so the kext's process_online
     * forwards a non-empty mode list to AppleParavirtFramebuffer::
     * connectionChange.
     *
     * RE: paravirt-re/annotated/AppleParavirtDisplayPipe-process_online
     *     .annotated.asm @ +0xe2:
     *       movzx   r11d, word [rax + 0x208]   ; numPixelFormats
     *     @ comment block:
     *       per-format record at ss[+0x210 + i*16]
     *         {u32 width, u32 height, u32 pixelFormat, u32 unknown}
     *     (the per-pixel-format query block @ 0x1456182c re-reads these
     *      on each getPixelInformation callback from IOFB)
     *
     * Without this, ss[+0x208]=0 from the bzero, the kext reports zero
     * formats, AppleParavirtFramebuffer publishes only its degenerate
     * 1x1 placeholder IOFBMode, IOFBConfig advertises IOFB0Hz=Yes, and
     * SkyLight's CompositorMetal::composite() aborts in
     * MetalShader::CopyPipelineState because the destination FB has
     * no valid mode. WindowServer dies before submitting any encType=2
     * render passes, which is why /tmp/lagfx.log only ever sees
     * encType=0 (kext-internal compute setup).
     *
     * The trailing 4 bytes of the record are RE-unverified; leave at 0.
     */
    uint16_t num_pixel_formats = 1u;
    dev->desc.shell.write_memory(
        dev->desc.shell.opaque, ss_gpa + 0x208u, sizeof(num_pixel_formats),
        &num_pixel_formats);

    uint8_t pixel_format_rec[16] = { 0 };
    /* +0x00 u32 width */
    lagfx_put_le32(pixel_format_rec + 0, 1920u);
    /* +0x04 u32 height */
    lagfx_put_le32(pixel_format_rec + 4, 1080u);
    /* +0x08 u32 pixelFormat — 'BGRA' fourcc (32-bit BGRA8888).
     * PGDisplayNub-presentSurface.annotated.asm line 213 confirms
     * ARGB / BGRA are the only accepted formats on the scanout path. */
    pixel_format_rec[8]  = 'B';
    pixel_format_rec[9]  = 'G';
    pixel_format_rec[10] = 'R';
    pixel_format_rec[11] = 'A';
    /* +0x0c..+0x0f unverified — leave zero */
    dev->desc.shell.write_memory(
        dev->desc.shell.opaque, ss_gpa + 0x210u, sizeof(pixel_format_rec),
        pixel_format_rec);

    LAGFX_LOG("vchan_setup_shared_state: display[%u] wrote mode "
              "(w=%u h=%u fb_len=%u fmt='BGRA' num_formats=%u)",
              display_index, 1920u, 1080u, fb_len,
              (unsigned)num_pixel_formats);

    /* Set pending online bit (ss[+0x100]=0x4) */
    uint32_t pending = 0x4u;
    dev->desc.shell.write_memory(
        dev->desc.shell.opaque, ss_gpa + 0x100u, sizeof(pending), &pending);

    return LAGFX_HANDLER_OK;
}

lagfx_handler_status_t lagfx_display_vchan_display_submit(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {

    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }

    uint32_t display_index = 0u;
    uint32_t arg2          = 0u;
    if (hdr->payload && hdr->payload_size >= 8u) {
        display_index = lagfx_le32(hdr->payload + 0);
        arg2          = lagfx_le32(hdr->payload + 4);
    }

    LAGFX_LOG("vchan_display_submit: display[%u] arg2=0x%08x stamp=0x%08x",
              display_index, arg2, hdr->stamp);

    /* Probe framebuffer to verify guest rendered content */
    lagfx_device_t *dev = (lagfx_device_t *)p->dev;
    if (arg2 != 0u && dev && dev->desc.shell.read_memory && display_index < 8u) {
        static unsigned probe_count;
        probe_count++;
        if (probe_count <= 4 || probe_count % 100 == 0) {
            uint64_t fb_base = (uint64_t)arg2 << 12;
            uint8_t probe[16] = {0};
            bool ok = dev->desc.shell.read_memory(
                dev->desc.shell.opaque, fb_base + 0x400u, 16, probe);
            uint32_t sum = 0;
            for (int i = 0; i < 16; ++i) {
                sum += probe[i];
            }
            LAGFX_LOG("vchan_display_submit: fb probe @0x%llx+0x400 ok=%d sum=%u bytes=%02x%02x...",
                      (unsigned long long)fb_base, ok, sum, probe[0], probe[1]);
        }
    }

    return LAGFX_HANDLER_OK;
}

lagfx_handler_status_t lagfx_display_define_child_fifo(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {

    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    if (!hdr->payload || hdr->payload_size < 44u) {
        LAGFX_WARN("vchan CmdDefineChildFIFO: payload too small (%u, need 44)", (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint64_t ring_base = lagfx_le64(hdr->payload + 8);
    uint64_t ring_size = lagfx_le64(hdr->payload + 16);
    uint32_t entry_count = lagfx_le32(hdr->payload + 24);

    LAGFX_LOG("vchan CmdDefineChildFIFO: display[%u] ring_base=0x%llx size=0x%x entries=%u stamp=0x%08x",
              p->current_chan_id - 5, (unsigned long long)ring_base, (uint32_t)ring_size, entry_count, hdr->stamp);

    /* TODO: Store child FIFO geometry for sub-channel command delivery */

    return LAGFX_HANDLER_OK;
}

lagfx_handler_status_t lagfx_display_vchan_present(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {

    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    if (!hdr->payload || hdr->payload_size < 12u) {
        LAGFX_WARN("vchan_present: payload too small (%u, need 12)", (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint32_t display_index = lagfx_le32(hdr->payload + 0);
    uint32_t surface_id    = lagfx_le32(hdr->payload + 4);
    uint32_t plane_id      = lagfx_le32(hdr->payload + 8);

    LAGFX_LOG("vchan_present: display[%u] surface=0x%x plane=%u stamp=0x%08x",
              display_index, surface_id, plane_id, hdr->stamp);

    /* TODO: Call lagfx_vk_display_present_surface() for Vulkan scanout */

    return LAGFX_HANDLER_OK;
}

lagfx_handler_status_t lagfx_display_vchan_present_gamma(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {

    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    if (!hdr->payload || hdr->payload_size < 36u) {
        LAGFX_WARN("vchan_present_gamma: payload too small (%u, need 36)", (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint32_t display_index = lagfx_le32(hdr->payload + 0);
    uint32_t plane_id      = lagfx_le32(hdr->payload + 4);
    uint32_t surface_id    = lagfx_le32(hdr->payload + 8);

    LAGFX_LOG("vchan_present_gamma: display[%u] surface=0x%x gamma_len=%u stamp=0x%08x",
              display_index, surface_id, lagfx_le32(hdr->payload + 20), hdr->stamp);

    /* TODO: Upload gamma table and call lagfx_vk_display_present_surface() */

    return LAGFX_HANDLER_OK;
}

/*
 * libapplegfx-vulkan — Display handlers (stub implementations)
 * src/handlers/display/display.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "../handlers.h"
#include "common/log.h"

/* Little-endian u16/u32/u64 readers (ring is LE on all hosts). */
static inline uint16_t lagfx_le16(const uint8_t *b) {
    return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}

static inline uint32_t lagfx_le32(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static inline uint64_t lagfx_le64(const uint8_t *b) {
    return (uint64_t)b[0] | ((uint64_t)b[1] << 8)
         | ((uint64_t)b[2] << 16) | ((uint64_t)b[3] << 24)
         | ((uint64_t)b[4] << 32) | ((uint64_t)b[5] << 40)
         | ((uint64_t)b[6] << 48) | ((uint64_t)b[7] << 56);
}

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

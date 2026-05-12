/*
 * libapplegfx-vulkan — Display handler stubs (0x10-0x1a)
 * src/handlers/display/display.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "../handlers/handlers.h"
#include "../common/log.h"

lagfx_handler_status_t lagfx_display_ack(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    (void)p;
    LAGFX_ERR("=== CmdDisplayAck CALLED ===");
    return LAGFX_HANDLER_OK; /* TODO: implement */
}

lagfx_handler_status_t lagfx_display_cursor_show(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    (void)p;
    LAGFX_ERR("=== CmdDisplayCursorShow CALLED ===");
    return LAGFX_HANDLER_OK; /* TODO: implement */
}

lagfx_handler_status_t lagfx_display_cursor_glyph(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    (void)p;
    LAGFX_ERR("=== CmdDisplayCursorGlyph CALLED ===");
    return LAGFX_HANDLER_OK; /* TODO: implement */
}

lagfx_handler_status_t lagfx_display_transaction3(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    (void)p;
    LAGFX_ERR("=== CmdDisplayTransaction3 CALLED ===");
    return LAGFX_HANDLER_OK; /* TODO: implement */
}

lagfx_handler_status_t lagfx_display_compositor_params(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    (void)p;
    LAGFX_ERR("=== CmdDisplayCompositorParameters CALLED ===");
    return LAGFX_HANDLER_OK; /* TODO: implement */
}

lagfx_handler_status_t lagfx_display_icc_profile(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    (void)p;
    LAGFX_ERR("=== CmdDisplaySetGuestICCProfile CALLED ===");
    return LAGFX_HANDLER_OK; /* TODO: implement */
}


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
    return lagfx_op_display_ack(p, hdr);
}

lagfx_handler_status_t lagfx_display_cursor_show(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    return lagfx_op_display_cursor_show(p, hdr);
}

lagfx_handler_status_t lagfx_display_cursor_glyph(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    return lagfx_op_display_cursor_glyph(p, hdr);
}

lagfx_handler_status_t lagfx_display_transaction3(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    return lagfx_op_display_transaction3(p, hdr);
}

lagfx_handler_status_t lagfx_display_compositor_params(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    return lagfx_op_display_compositor_params(p, hdr);
}

lagfx_handler_status_t lagfx_display_icc_profile(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    return lagfx_op_display_set_icc_profile(p, hdr);
}

/* === VChan handler stubs (pre-existing bug - implementations missing) ===== */
/* These were removed in commit 1edf7ee but still referenced. Adding stubs to allow build. */

lagfx_handler_status_t lagfx_display_vchan_setup_shared_state(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    (void)p; (void)hdr;
    return LAGFX_HANDLER_OK; /* TODO: restore implementation */
}

lagfx_handler_status_t lagfx_display_vchan_display_submit(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    (void)p; (void)hdr;
    return LAGFX_HANDLER_OK; /* TODO: restore implementation */
}

lagfx_handler_status_t lagfx_display_vchan_present(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    (void)p; (void)hdr;
    return LAGFX_HANDLER_OK; /* TODO: restore implementation */
}

lagfx_handler_status_t lagfx_display_vchan_present_gamma(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    (void)p; (void)hdr;
    return LAGFX_HANDLER_OK; /* TODO: restore implementation */
}

lagfx_handler_status_t lagfx_display_define_child_fifo(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    (void)p; (void)hdr;
    return LAGFX_HANDLER_OK; /* TODO: restore implementation */
}

/*
 * libapplegfx-vulkan — Utility handler stubs (0x11, 0x0a)
 * src/handlers/util/util.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "../handlers/handlers.h"
#include "../common/log.h"

lagfx_handler_status_t lagfx_util_nop(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    (void)p;
    LAGFX_TRACE("handler: executing logic function");
    return LAGFX_HANDLER_OK; /* NOP - always success */
}

lagfx_handler_status_t lagfx_util_device_info(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    (void)p;
    LAGFX_TRACE("handler: executing logic function");
    return LAGFX_HANDLER_OK; /* TODO: implement */
}


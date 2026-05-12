/*
 * libapplegfx-vulkan — Memory handler stubs (0x02-0x05)
 * src/handlers/memory/memory.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "../handlers/handlers.h"
#include "../common/log.h"

lagfx_handler_status_t lagfx_memory_map_memory2(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    (void)p;
    LAGFX_ERR("=== CMD_MAP_MEMORY_2 CALLED ===");
    return LAGFX_HANDLER_OK; /* TODO: implement */
}

lagfx_handler_status_t lagfx_memory_unmap_memory(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    (void)p;
    LAGFX_ERR("=== CMD_UNMAP_MEMORY CALLED ===");
    return LAGFX_HANDLER_OK; /* TODO: implement */
}

lagfx_handler_status_t lagfx_memory_define_child_fifo(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    (void)p;
    LAGFX_ERR("=== CMD_DEFINE_CHILD_FIFO CALLED ===");
    return LAGFX_HANDLER_OK; /* TODO: implement */
}

lagfx_handler_status_t lagfx_memory_delete_child_fifo(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    (void)p;
    LAGFX_ERR("=== CMD_DELETE_CHILD_FIFO CALLED ===");
    return LAGFX_HANDLER_OK; /* TODO: implement */
}


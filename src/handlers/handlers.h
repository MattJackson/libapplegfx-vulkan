/*
 * libapplegfx-vulkan — Handler declarations
 * src/handlers/handlers.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef LAGFX_HANDLERS_H
#define LAGFX_HANDLERS_H

#include "../device.h"
#include "protocol/ops_display.h"
#include "protocol/resource_registry.h"
#include "protocol/opcodes.h"
#include "protocol/state.h"

/* === Task Handlers (opcode 0x00, 0x01) ============================ */
lagfx_handler_status_t lagfx_task_define_task2(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_task_delete_task(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);

/* === Memory Handlers (opcode 0x02-0x03) ========================== */
lagfx_handler_status_t lagfx_memory_map_memory2(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_memory_unmap_memory(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);

/* === Compute Handler (opcode 0x20) =============================== */
lagfx_handler_status_t lagfx_compute_exec_cmdbuf(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);

/* Legacy alias for backward compatibility. */
static inline lagfx_handler_status_t lagfx_compute_exec_indirect2(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    return lagfx_compute_exec_cmdbuf(p, hdr);
}

/* === Sync Handlers (opcode 0x42) ================================ */
lagfx_handler_status_t lagfx_sync_synchronize_resources(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);

/* === Display Handlers (opcode 0x10-0x1a) ======================== */
lagfx_handler_status_t lagfx_display_ack(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_display_swap_mapping(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_display_cursor_glyph(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_display_cursor_show(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);

/* === Utility Handlers ============================================ */
lagfx_handler_status_t lagfx_util_nop(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_op_debug(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_op_get_device_info(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_op_get_device_info_2(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);

#endif /* LAGFX_HANDLERS_H */

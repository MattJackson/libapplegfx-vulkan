/*
 * libapplegfx-vulkan — display virtual-channel opcode handlers
 * src/protocol/ops_display_vchan.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Display vchan opcodes (ch >= 5) use a separate namespace from the
 * root FIFO.  Handlers are dispatched directly by the per-channel
 * doorbell loop in protocol.c, NOT through the root opcode table.
 *
 *   0x01  setupSharedState   8  bytes payload
 *   0x02  display submit      8  bytes payload
 *   0x06  present            12 bytes payload
 *   0x07  present+gamma      36 bytes payload
 *
 * Private to src/protocol/. Not installed.
 */

#ifndef LIBAPPLEGFX_PROTOCOL_OPS_DISPLAY_VCHAN_H
#define LIBAPPLEGFX_PROTOCOL_OPS_DISPLAY_VCHAN_H

#include "opcodes.h"

lagfx_handler_status_t lagfx_op_vchan_setup_shared_state(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);

lagfx_handler_status_t lagfx_op_vchan_display_submit(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);

lagfx_handler_status_t lagfx_op_vchan_present(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);

lagfx_handler_status_t lagfx_op_vchan_present_gamma(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);

lagfx_handler_status_t lagfx_op_display_define_child_fifo(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);

lagfx_handler_status_t lagfx_op_vchan_unknown_extended(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr, uint8_t opcode);

#endif

/*
 * libapplegfx-vulkan — Display channel dispatcher (ch 5+)
 * src/dispatchers/channel_display_dispatcher.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "channel_display_dispatcher.h"
#include "../doorbell.h"
#include "../device.h"
#include "../common/log.h"
#include "protocol/opcodes.h"
#include "protocol/state.h"
#include "handlers/handlers.h"

#include <stddef.h>

/* Little-endian readers (guest protocol is LE). */
static inline uint16_t read_le16(const uint8_t *b) {
    return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}

static inline uint32_t read_le32(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

/* Dispatch a single command to appropriate handler. */
static void dispatch_command(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return;
    }

    LAGFX_TRACE("display dispatch: opcode=0x%04x stamp=0x%08x", hdr->opcode, hdr->stamp);

    switch (hdr->opcode) {
        /* VChan display opcodes (compact namespace for ch 5+) */
        case 0x01u:  // setupSharedState
            lagfx_display_vchan_setup_shared_state(p, hdr);
            break;
        case 0x02u:  // displaySubmit
            lagfx_display_vchan_display_submit(p, hdr);
            break;
        case 0x04u:  // CmdDefineChildFIFO
            lagfx_display_define_child_fifo(p, hdr);
            break;
        case 0x06u:  // present
            lagfx_display_vchan_present(p, hdr);
            break;
        case 0x07u:  // present+gamma
            lagfx_display_vchan_present_gamma(p, hdr);
            break;

        /* Display ack and state */
        case LAGFX_OP_DISPLAY_ACK:
            lagfx_display_ack(p, hdr);
            break;
        case LAGFX_OP_DISPLAY_SWAP_MAPPING:
            lagfx_display_swap_mapping(p, hdr);
            break;

        /* Cursor rendering */
        case LAGFX_OP_DISPLAY_CURSOR_GLYPH:
            lagfx_display_cursor_glyph(p, hdr);
            break;
        case LAGFX_OP_DISPLAY_CURSOR_SHOW:
            lagfx_display_cursor_show(p, hdr);
            break;

        default:
            LAGFX_WARN("display dispatch: unknown opcode 0x%04x", hdr->opcode);
            p->unknown_opcode_count++;
            break;
    }
}

/* Drain ring buffer for display channels. Returns number of commands processed. */
size_t channel_display_dispatcher_drain(lagfx_protocol_t *p, uint32_t chan_id) {
    if (!lagfx_protocol_is_valid(p)) {
        return 0;
    }

    /* TODO: Get per-channel ring geometry from FIFO entry or protocol state. */
    if (!p->ring_armed || p->ring_size == 0u || p->ring_base_gpa == 0u) {
        LAGFX_TRACE("display drain: ring not armed");
        return 0;
    }

    /* Read doorbell for this channel. */
    uint32_t write_ptr = p->reg[REG_WRITE_PTR + chan_id];
    if (write_ptr == 0u) {
        return 0;
    }

    LAGFX_LOG("display drain: chan=%u doorbell=%u", chan_id, write_ptr);

    size_t cmds_processed = 0u;
    const size_t max_cmds = 64u;
    uint64_t ring_base_gpa = p->ring_base_gpa;
    size_t ring_size       = p->ring_size;
    uint32_t read_ptr      = 0u;

    for (size_t i = 0; i < max_cmds && cmds_processed < write_ptr; ++i) {
        /* Calculate command GPA. */
        uint64_t cmd_gpa = ring_base_gpa + (read_ptr % ring_size);

        /* Fetch 12-byte header from guest via shell.read_memory callback. */
        lagfx_cmd_header_wire_t wire_hdr;
        if (!p->dev || !((lagfx_device_t *)p->dev)->desc.shell.read_memory) {
            LAGFX_WARN("display drain: no shell.read_memory callback available");
            break;
        }

        bool ok = ((lagfx_device_t *)p->dev)->desc.shell.read_memory(
            ((lagfx_device_t *)p->dev)->desc.shell.opaque,
            cmd_gpa, sizeof(wire_hdr), &wire_hdr);

        if (!ok) {
            LAGFX_WARN("display drain: read_memory failed for GPA 0x%llx", (unsigned long long)cmd_gpa);
            break;
        }

        /* Convert wire header to derived format. */
        lagfx_cmd_header_t hdr;
        hdr.opcode       = wire_hdr.opcode;
        hdr.length       = wire_hdr.length;
        hdr.stamp        = wire_hdr.stamp;
        hdr.arg_count_8b = wire_hdr.arg_count_8b;
        hdr.payload_size = (uint16_t)(wire_hdr.length > 12 ? wire_hdr.length - 12 : 0);

        /* TODO: Fetch payload via shell.read_memory if length > 12. */

        /* Dispatch command to handler. */
        dispatch_command(p, &hdr);

        p->total_cmds_seen++;
        cmds_processed++;
        read_ptr++;
    }

    return cmds_processed;
}

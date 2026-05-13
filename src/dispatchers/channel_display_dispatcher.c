/*
 * libapplegfx-vulkan — Display channel dispatcher (ch 5+)
 * src/dispatchers/channel_display_dispatcher.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Display vchan drain. Wire format and read semantics mirror the
 * root-channel drainer; opcodes are the compact vchan namespace
 * (0x01-0x07) plus a few of the §3 opcodes that fire on display
 * channels (cursor, ack, swap).
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

#define LAGFX_DISP_DRAIN_MAX_CMDS 128u
#define LAGFX_DISP_MAX_CMD_BYTES  4096u

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

    if (!p->ring_armed || p->ring_size == 0u || p->ring_base_gpa == 0u) {
        LAGFX_TRACE("display drain: ring not armed");
        return 0;
    }

    lagfx_device_t *dev = (lagfx_device_t *)p->dev;
    if (!dev || !dev->desc.shell.read_memory) {
        LAGFX_WARN("display drain: no shell.read_memory callback");
        return 0;
    }

    /* Display channels use sub-channel rings registered via
     * CmdDefineChildFIFO. Until those are wired into protocol state
     * we drain from the primary ring geometry on a best-effort
     * basis so doorbells make some progress for diagnostics. */
    uint32_t write_ptr = p->reg[REG_WRITE_PTR];
    if (write_ptr == 0u) {
        return 0;
    }

    LAGFX_LOG("display drain: chan=%u wp=0x%x", chan_id, write_ptr);

    uint64_t ring_base = p->ring_base_gpa;
    uint32_t ring_size = p->ring_size;
    uint32_t rp        = 0u;

    size_t cmds = 0;
    for (unsigned i = 0; i < LAGFX_DISP_DRAIN_MAX_CMDS; ++i) {
        if (rp == write_ptr) break;

        uint32_t rp_mod = rp % ring_size;
        uint64_t hdr_gpa = ring_base + (uint64_t)rp_mod;

        uint8_t hdr_buf[LAGFX_CMD_HEADER_BYTES];
        if (!dev->desc.shell.read_memory(dev->desc.shell.opaque,
                                          hdr_gpa,
                                          LAGFX_CMD_HEADER_BYTES,
                                          hdr_buf)) {
            LAGFX_WARN("display drain: header DMA failed");
            break;
        }

        uint16_t opcode       = read_le16(hdr_buf + 0);
        uint16_t arg_count_8b = read_le16(hdr_buf + 2);
        uint32_t length       = read_le32(hdr_buf + 4);
        uint32_t stamp        = read_le32(hdr_buf + 8);

        if (length < LAGFX_CMD_HEADER_BYTES ||
            length > LAGFX_DISP_MAX_CMD_BYTES ||
            length > ring_size) {
            LAGFX_WARN("display drain: bad length 0x%x at rp=0x%x — stop",
                       length, rp_mod);
            break;
        }

        uint8_t cmd_buf[LAGFX_DISP_MAX_CMD_BYTES];
        uint32_t head_len = ring_size - rp_mod;
        if (head_len >= length) {
            if (!dev->desc.shell.read_memory(dev->desc.shell.opaque,
                                              hdr_gpa, length, cmd_buf)) break;
        } else {
            if (!dev->desc.shell.read_memory(dev->desc.shell.opaque,
                                              hdr_gpa, head_len, cmd_buf)) break;
            if (!dev->desc.shell.read_memory(dev->desc.shell.opaque,
                                              ring_base,
                                              length - head_len,
                                              cmd_buf + head_len)) break;
        }

        lagfx_cmd_header_t hdr;
        hdr.opcode       = opcode;
        hdr.arg_count_8b = arg_count_8b;
        hdr.length       = length;
        hdr.stamp        = stamp;
        hdr.payload_size = (uint16_t)((length > LAGFX_CMD_HEADER_BYTES)
                                       ? (length - LAGFX_CMD_HEADER_BYTES)
                                       : 0u);
        hdr.payload      = (hdr.payload_size > 0)
                              ? (cmd_buf + LAGFX_CMD_HEADER_BYTES)
                              : NULL;

        dispatch_command(p, &hdr);

        /* Display slots are chan_id (5+); see SLOT_DISPLAY_PIPE_0. */
        lagfx_protocol_complete_stamp_slot(p, chan_id, stamp);

        p->total_cmds_seen++;
        cmds++;
        rp = (rp_mod + length) % ring_size;
    }

    return cmds;
}

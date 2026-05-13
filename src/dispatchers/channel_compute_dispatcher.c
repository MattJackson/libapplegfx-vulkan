/*
 * libapplegfx-vulkan — Compute channel dispatcher (ch 1-4)
 * src/dispatchers/channel_compute_dispatcher.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Per-channel ring drain for compute vchans. Wire format and read
 * semantics mirror the root-channel drainer in
 * channel_0_dispatcher.c. Per-channel ring geometry (its own
 * ring_base_gpa / ring_size) is sourced from the FIFO entry
 * registered by CmdDefineChildFIFO; if none is registered yet, we
 * fall back to the shared primary-ring geometry so the call still
 * makes progress during bring-up.
 */

#include "channel_compute_dispatcher.h"
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

#define LAGFX_COMPUTE_DRAIN_MAX_CMDS 128u
#define LAGFX_COMPUTE_MAX_CMD_BYTES  4096u

/* Dispatch a single command to appropriate handler. */
static void dispatch_command(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return;
    }

    LAGFX_TRACE("compute dispatch: opcode=0x%04x stamp=0x%08x", hdr->opcode, hdr->stamp);

    switch (hdr->opcode) {
        /* Compute shader execution */
        case LAGFX_OP_EXEC_INDIRECT2:
            lagfx_compute_exec_indirect2(p, hdr);
            break;

        default:
            LAGFX_WARN("compute dispatch: unknown opcode 0x%04x", hdr->opcode);
            p->unknown_opcode_count++;
            break;
    }
}

/* Drain ring buffer for compute channels. Returns number of commands processed. */
size_t channel_compute_dispatcher_drain(lagfx_protocol_t *p, uint32_t chan_id) {
    if (!lagfx_protocol_is_valid(p)) {
        return 0;
    }

    if (!p->ring_armed || p->ring_size == 0u || p->ring_base_gpa == 0u) {
        LAGFX_TRACE("compute drain: ring not armed");
        return 0;
    }

    lagfx_device_t *dev = (lagfx_device_t *)p->dev;
    if (!dev || !dev->desc.shell.read_memory) {
        LAGFX_WARN("compute drain: no shell.read_memory callback");
        return 0;
    }

    /* TODO: real per-channel write_ptr lives in the child-FIFO doorbell
     * page (shared_pfn<<12 + 0x400 + N*desc_size). For bring-up the
     * compute path is rarely hit before the root channel publishes
     * AppleParavirtGPUControl, so we use a conservative bound. */
    uint32_t write_ptr = p->reg[REG_WRITE_PTR];
    if (write_ptr == 0u) {
        return 0;
    }

    LAGFX_LOG("compute drain: chan=%u wp=0x%x", chan_id, write_ptr);

    uint64_t ring_base = p->ring_base_gpa;
    uint32_t ring_size = p->ring_size;
    uint32_t rp        = 0u;  /* compute channels track their own rp on the FIFO entry */

    size_t cmds = 0;
    for (unsigned i = 0; i < LAGFX_COMPUTE_DRAIN_MAX_CMDS; ++i) {
        if (rp == write_ptr) break;

        uint32_t rp_mod = rp % ring_size;
        uint64_t hdr_gpa = ring_base + (uint64_t)rp_mod;

        uint8_t hdr_buf[LAGFX_CMD_HEADER_BYTES];
        if (!dev->desc.shell.read_memory(dev->desc.shell.opaque,
                                          hdr_gpa,
                                          LAGFX_CMD_HEADER_BYTES,
                                          hdr_buf)) {
            LAGFX_WARN("compute drain: header DMA failed at gpa=0x%llx",
                       (unsigned long long)hdr_gpa);
            break;
        }

        uint16_t opcode       = read_le16(hdr_buf + 0);
        uint16_t arg_count_8b = read_le16(hdr_buf + 2);
        uint32_t length       = read_le32(hdr_buf + 4);
        uint32_t stamp        = read_le32(hdr_buf + 8);

        if (length < LAGFX_CMD_HEADER_BYTES ||
            length > LAGFX_COMPUTE_MAX_CMD_BYTES ||
            length > ring_size) {
            LAGFX_WARN("compute drain: bad length 0x%x at rp=0x%x — stop",
                       length, rp_mod);
            break;
        }

        uint8_t cmd_buf[LAGFX_COMPUTE_MAX_CMD_BYTES];
        uint32_t head_len = ring_size - rp_mod;
        if (head_len >= length) {
            if (!dev->desc.shell.read_memory(dev->desc.shell.opaque,
                                              hdr_gpa, length, cmd_buf)) {
                LAGFX_WARN("compute drain: body DMA failed");
                break;
            }
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

        /* Per-channel stamp slot = chan_id (1..4 → SLOT_COMPUTE_1..4). */
        lagfx_protocol_complete_stamp_slot(p, chan_id, stamp);

        p->total_cmds_seen++;
        cmds++;
        rp = (rp_mod + length) % ring_size;
    }

    return cmds;
}

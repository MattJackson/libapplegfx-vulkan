/*
 * libapplegfx-vulkan — Root channel dispatcher (ch 0)
 * src/dispatchers/channel_0_dispatcher.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Drains the root command ring on a BAR0+0x1008 doorbell. read_ptr /
 * write_ptr are BYTE OFFSETS within the ring (not command counts);
 * each command's 12-byte header is laid out on the wire as:
 *
 *   off 0: u16 opcode
 *   off 2: u16 arg_count_8b
 *   off 4: u32 length         (total bytes, header + payload)
 *   off 8: u32 stamp
 *
 * payload (length - 12 bytes) follows the header inline in the ring.
 * Wrap is handled with a two-DMA read when a command straddles
 * ring_size. See the legacy lagfx_fifo_drain in
 * git show af87e8c~1:src/protocol/fifo.c — this is the same shape,
 * adapted to the new lagfx_protocol_t layout and handler tables.
 */

#include "channel_0_dispatcher.h"
#include "../device.h"
#include "../doorbell.h"
#include "../common/log.h"
#include "protocol/opcodes.h"
#include "protocol/state.h"
#include "handlers/handlers.h"

#include <stdio.h>
#include <string.h>

/* Little-endian readers (guest protocol is LE on x86-64). */
static inline uint16_t read_le16(const uint8_t *b) {
    return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}

static inline uint32_t read_le32(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

/* Sanity cap matches the legacy drainer. 4 KiB per command is well
 * above any single legitimate macOS command we've seen. */
#define LAGFX_CH0_DRAIN_MAX_CMDS 128u
#define LAGFX_CH0_MAX_CMD_BYTES  4096u

/* Dispatch a single command to the appropriate handler. Handlers
 * return a status; on OK (or fail-open SIZE/STATE errors) we still
 * raise the stamp so the guest doesn't park forever in waitForStamp.
 */
static void dispatch_command(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return;
    }

    LAGFX_TRACE("ch0 dispatch: opcode=0x%04x len=%u stamp=0x%08x",
                hdr->opcode, hdr->length, hdr->stamp);

    switch (hdr->opcode) {
        /* Task management */
        case LAGFX_OP_DEFINE_TASK2:
            lagfx_task_define_task2(p, hdr);
            break;
        case LAGFX_OP_DELETE_TASK:
            lagfx_task_delete_task(p, hdr);
            break;

        /* Memory mapping */
        case LAGFX_OP_MAP_MEMORY2:
            lagfx_memory_map_memory2(p, hdr);
            break;
        case LAGFX_OP_UNMAP_MEMORY:
            lagfx_memory_unmap_memory(p, hdr);
            break;

        /* Device info queries */
        case LAGFX_OP_GET_DEVICE_INFO:
            lagfx_op_get_device_info(p, hdr);
            break;
        case LAGFX_OP_GET_DEVICE_INFO_2:
            lagfx_op_get_device_info_2(p, hdr);
            break;

        /* Debug/NOP */
        case LAGFX_OP_NOP:
            lagfx_util_nop(p, hdr);
            break;
        case LAGFX_OP_DEBUG:
            lagfx_op_debug(p, hdr);
            break;

        default:
            LAGFX_WARN("ch0 dispatch: unknown opcode 0x%04x stamp=0x%08x",
                       hdr->opcode, hdr->stamp);
            p->unknown_opcode_count++;
            break;
    }
}

/* Drain the root channel ring. Returns the number of commands
 * processed. read_ptr and write_ptr are byte offsets into the ring;
 * each command advances read_ptr by hdr.length, wrapping at
 * ring_size. Each completion raises the slot-0 stamp + MSI so the
 * kext's waitForStamp() can return.
 */
size_t channel_0_dispatcher_drain(lagfx_protocol_t *p) {
    if (!lagfx_protocol_is_valid(p)) {
        return 0;
    }

    if (!p->ring_armed || p->ring_size == 0u || p->ring_base_gpa == 0u) {
        LAGFX_TRACE("ch0 drain: ring not armed (armed=%d size=0x%x gpa=0x%llx)",
                    (int)p->ring_armed, p->ring_size,
                    (unsigned long long)p->ring_base_gpa);
        return 0;
    }

    lagfx_device_t *dev = (lagfx_device_t *)p->dev;
    if (!dev || !dev->desc.shell.read_memory) {
        LAGFX_WARN("ch0 drain: no shell.read_memory callback");
        return 0;
    }

    uint32_t write_ptr = p->write_ptr;
    uint32_t ring_size = p->ring_size;
    uint64_t ring_base = p->ring_base_gpa;

    if (p->read_ptr == write_ptr) {
        LAGFX_TRACE("ch0 drain: caught up rp=0x%x wp=0x%x", p->read_ptr, write_ptr);
        return 0;
    }

    LAGFX_LOG("ch0 drain: rp=0x%x wp=0x%x ring_size=0x%x base_gpa=0x%llx",
              p->read_ptr, write_ptr, ring_size,
              (unsigned long long)ring_base);

    size_t cmds = 0;
    for (unsigned i = 0; i < LAGFX_CH0_DRAIN_MAX_CMDS; ++i) {
        if (p->read_ptr == write_ptr) {
            break;  /* caught up */
        }

        uint32_t rp = p->read_ptr % ring_size;
        uint64_t hdr_gpa = ring_base + (uint64_t)rp;

        /* Step 1: read the 12-byte header. The header itself never
         * wraps because rings are aligned and headers are 12 bytes;
         * a wrap mid-header would mean the producer wrote a
         * malformed entry. We still defensively wrap-read the body
         * below. */
        uint8_t hdr_buf[LAGFX_CMD_HEADER_BYTES];
        if (!dev->desc.shell.read_memory(dev->desc.shell.opaque,
                                          hdr_gpa,
                                          LAGFX_CMD_HEADER_BYTES,
                                          hdr_buf)) {
            LAGFX_WARN("ch0 drain: header DMA failed at gpa=0x%llx",
                       (unsigned long long)hdr_gpa);
            break;
        }

        uint16_t opcode       = read_le16(hdr_buf + 0);
        uint16_t arg_count_8b = read_le16(hdr_buf + 2);
        uint32_t length       = read_le32(hdr_buf + 4);
        uint32_t stamp        = read_le32(hdr_buf + 8);

        if (length < LAGFX_CMD_HEADER_BYTES ||
            length > LAGFX_CH0_MAX_CMD_BYTES ||
            length > ring_size) {
            LAGFX_WARN("ch0 drain: bad length 0x%x at rp=0x%x opcode=0x%04x — stop",
                       length, rp, opcode);
            break;
        }

        /* Step 2: read the full command (header + payload) into a
         * local buffer so handlers can chase payload->payload_size.
         * Handles wrap by a second DMA for the tail. */
        uint8_t cmd_buf[LAGFX_CH0_MAX_CMD_BYTES];
        uint32_t head_len = ring_size - rp;
        if (head_len >= length) {
            if (!dev->desc.shell.read_memory(dev->desc.shell.opaque,
                                              hdr_gpa, length, cmd_buf)) {
                LAGFX_WARN("ch0 drain: body DMA failed at gpa=0x%llx len=%u",
                           (unsigned long long)hdr_gpa, length);
                break;
            }
        } else {
            if (!dev->desc.shell.read_memory(dev->desc.shell.opaque,
                                              hdr_gpa, head_len, cmd_buf)) {
                LAGFX_WARN("ch0 drain: wrapped head DMA failed");
                break;
            }
            if (!dev->desc.shell.read_memory(dev->desc.shell.opaque,
                                              ring_base,
                                              length - head_len,
                                              cmd_buf + head_len)) {
                LAGFX_WARN("ch0 drain: wrapped tail DMA failed");
                break;
            }
        }

        /* Build derived header. payload pointer is just past the
         * 12-byte on-wire header; payload_size is length - 12. */
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

        /* Hex dump of first 32 bytes on TRACE for bring-up debugging. */
        if (lagfx_log_level() >= LAGFX_LOG_LVL_TRACE) {
            uint32_t dump_len = length < 32u ? length : 32u;
            char hexline[128];
            size_t pos = 0;
            for (uint32_t j = 0; j < dump_len && pos + 4 < sizeof(hexline); ++j) {
                int n = snprintf(hexline + pos, sizeof(hexline) - pos,
                                 "%02x ", cmd_buf[j]);
                if (n < 0 || (size_t)n >= sizeof(hexline) - pos) break;
                pos += (size_t)n;
            }
            hexline[pos] = '\0';
            LAGFX_TRACE("ch0 drain: bytes[%u] %s", dump_len, hexline);
        }

        dispatch_command(p, &hdr);

        /* Every root-channel command unconditionally signals its
         * stamp on completion (see paravirt-re
         * waitForStamp-mechanism-summary.md). */
        lagfx_protocol_complete_stamp(p, stamp);

        p->total_cmds_seen++;
        cmds++;
        p->read_ptr = (rp + length) % ring_size;
    }

    LAGFX_LOG("ch0 drain: drained=%zu new rp=0x%x", cmds, p->read_ptr);
    return cmds;
}

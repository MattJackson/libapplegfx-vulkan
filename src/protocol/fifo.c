/*
 * libapplegfx-vulkan — FIFO ring dequeue (Phase 1.A.2 skeleton)
 * src/protocol/fifo.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The command header is 12 bytes (see opcodes.h and
 * re-followup-spec-gaps.md §5.1). The doorbell is a write-pointer
 * update, not a stamp, and lives at an MMIO offset in the
 * 0x1004..0x1034 range that we haven't identified yet.
 *
 * This TU owns:
 *   - lagfx_fifo_parse_header: decodes the 12-byte on-wire header
 *     into lagfx_cmd_header_t. Unit-tested.
 *   - lagfx_fifo_drain: ring-buffer walk driven by the 0x1008 doorbell.
 */

#include "fifo.h"
#include "protocol.h"
#include "state.h"
#include "opcodes.h"
#include "../common/log.h"
#include "../device.h"

#include <string.h>

/* Sanity cap: one ring has hundreds of bytes of commands at most during
 * any single drain; 128 commands × 4 KiB is an absurd ceiling that
 * still protects against a runaway guest writing garbage. Raise later
 * if real workloads need more. */
#define LAGFX_FIFO_DRAIN_MAX_CMDS 128u
#define LAGFX_FIFO_MAX_CMD_BYTES  4096u

/* Little-endian u16 / u32 readers. Guest protocol is x86-64 LE per
 * command-buffer-format.md §2. We do a per-byte read so this compiles
 * to something sensible regardless of host alignment rules. */
static inline uint16_t read_le16(const uint8_t *b) {
    return (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}
static inline uint32_t read_le32(const uint8_t *b) {
    return (uint32_t)b[0]
         | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16)
         | ((uint32_t)b[3] << 24);
}

bool lagfx_fifo_parse_header(const uint8_t *bytes, size_t len,
                             lagfx_cmd_header_t *hdr_out) {
    if (!bytes || !hdr_out || len < LAGFX_CMD_HEADER_BYTES) {
        return false;
    }

    uint16_t opcode       = read_le16(bytes + 0);
    uint16_t arg_count_8b = read_le16(bytes + 2);
    uint32_t total_length = read_le32(bytes + 4);
    uint32_t stamp        = read_le32(bytes + 8);

    /* Sanity: length must be at least the header size. */
    if (total_length < LAGFX_CMD_HEADER_BYTES) {
        return false;
    }

    uint32_t payload_bytes = total_length - LAGFX_CMD_HEADER_BYTES;

    /* The caller may have only the 12 header bytes; flag that via
     * payload==NULL but still report payload_size so handlers can
     * distinguish "no payload" from "payload was elided". */
    const uint8_t *payload = NULL;
    if (payload_bytes > 0 && len >= (size_t)total_length) {
        payload = bytes + LAGFX_CMD_HEADER_BYTES;
    }

    hdr_out->opcode       = opcode;
    hdr_out->arg_count_8b = arg_count_8b;
    hdr_out->length       = total_length;
    hdr_out->stamp        = stamp;
    /* payload_size is declared u16; clamp the derived value. In
     * practice `length` is bounded by the ring size (128 KB), so this
     * clamp only triggers on malformed input. */
    hdr_out->payload_size =
        (payload_bytes > 0xffffu) ? 0xffffu : (uint16_t)payload_bytes;
    hdr_out->payload      = payload;

    return true;
}

size_t lagfx_fifo_drain(lagfx_protocol_t *p) {
    if (!lagfx_protocol_is_valid(p)) {
        return 0;
    }

    if (!p->ring_armed || p->ring_size == 0u || p->ring_base_gpa == 0u) {
        LAGFX_TRACE("fifo_drain: ring not armed (armed=%d size=0x%x gpa=0x%llx)",
                  (int)p->ring_armed, p->ring_size,
                  (unsigned long long)p->ring_base_gpa);
        return 0;
    }

    if (!p->dev || !p->dev->desc.shell.read_memory) {
        LAGFX_WARN("fifo_drain: no shell.read_memory callback");
        return 0;
    }

    /* Standard ring-buffer drain: advance read_ptr toward write_ptr,
     * parsing each 12-byte header + payload in place, dispatching via
     * the opcode handler table. Stop when read_ptr catches write_ptr
     * or on malformed data (defensive). Handles ring wrap naturally
     * via modulo arithmetic on read_ptr.
     */
    size_t drained = 0;
    for (unsigned i = 0; i < LAGFX_FIFO_DRAIN_MAX_CMDS; ++i) {
        if (p->read_ptr == p->write_ptr) {
            break;  /* caught up */
        }

        /* Wrap at the ring boundary. */
        uint32_t rp = p->read_ptr % p->ring_size;
        uint64_t hdr_gpa = p->ring_base_gpa + (uint64_t)rp;

        uint8_t hdr_buf[LAGFX_CMD_HEADER_BYTES];
        if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                            hdr_gpa,
                                            LAGFX_CMD_HEADER_BYTES,
                                            hdr_buf)) {
            LAGFX_WARN("fifo_drain: DMA read of header at gpa=0x%llx failed",
                       (unsigned long long)hdr_gpa);
            break;
        }

        lagfx_cmd_header_t hdr;
        if (!lagfx_fifo_parse_header(hdr_buf, LAGFX_CMD_HEADER_BYTES,
                                     &hdr)) {
            LAGFX_TRACE("fifo_drain: malformed header at rp=0x%x — stop", rp);
            break;
        }

        if (hdr.length < LAGFX_CMD_HEADER_BYTES ||
            hdr.length > LAGFX_FIFO_MAX_CMD_BYTES ||
            hdr.length > p->ring_size) {
            LAGFX_WARN("fifo_drain: bad length at rp=0x%x (len=0x%x) — stop",
                       rp, hdr.length);
            break;
        }

        /* Read the whole command (header + payload) into a local buf.
         * Handles ring wrap by a second DMA for the tail when needed. */
        uint8_t cmd_buf[LAGFX_FIFO_MAX_CMD_BYTES];
        uint32_t head = p->ring_size - rp;
        if (head >= hdr.length) {
            if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                                hdr_gpa, hdr.length,
                                                cmd_buf)) {
                LAGFX_WARN("fifo_drain: DMA read of cmd body failed");
                break;
            }
        } else {
            if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                                hdr_gpa, head, cmd_buf)) {
                LAGFX_WARN("fifo_drain: DMA read of wrapped head failed");
                break;
            }
            if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                                p->ring_base_gpa,
                                                hdr.length - head,
                                                cmd_buf + head)) {
                LAGFX_WARN("fifo_drain: DMA read of wrapped tail failed");
                break;
            }
        }

        LAGFX_TRACE("fifo_drain: dispatch rp=0x%x opcode=0x%x len=0x%x stamp=0x%x",
                    rp, hdr.opcode, hdr.length, hdr.stamp);

        /* Hex dump of the full command (up to 32 bytes) for stamp-field
         * and payload cross-referencing during M3 bring-up. Resolves the
         * contradiction between spec-gaps §13 (stamps in trace 1..7) and
         * the static RE claim that `[stream+8..11]` is always zero. */
        if (lagfx_log_level() >= LAGFX_LOG_LVL_TRACE) {
            uint32_t dump_len = hdr.length < 32u ? hdr.length : 32u;
            char hexline[128];
            size_t pos = 0;
            for (uint32_t i = 0; i < dump_len && pos + 4 < sizeof(hexline); ++i) {
                int n = snprintf(hexline + pos, sizeof(hexline) - pos,
                                 "%02x ", cmd_buf[i]);
                if (n < 0 || (size_t)n >= sizeof(hexline) - pos) break;
                pos += (size_t)n;
            }
            hexline[pos] = '\0';
            LAGFX_TRACE("fifo_drain: bytes[%u] %s", dump_len, hexline);
        }

        /* Expose ring-header GPA to handlers so they can DMA-write
         * header slots (e.g. 0x3a's actual_count) BEFORE dispatch_one
         * fires the stamp + IRQ — otherwise the guest services the
         * IRQ and reads stale bytes. */
        p->current_cmd_header_gpa = hdr_gpa;

        (void)lagfx_protocol_dispatch_one(p, cmd_buf, hdr.length);

        p->current_cmd_header_gpa = 0;

        p->read_ptr = (rp + hdr.length) % p->ring_size;
        drained += 1;
    }

    LAGFX_TRACE("fifo_drain: drained=%zu, new read_ptr=0x%x",
              drained, p->read_ptr);
    return drained;
}

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
 * This TU owns two real pieces of code:
 *   - lagfx_fifo_parse_header: decodes the 12-byte on-wire header
 *     into lagfx_cmd_header_t. Unit-tested.
 *   - lagfx_fifo_on_mmio_setter: probe handler for any MMIO write
 *     in the setter-candidate range. Logs (offset, value) and records
 *     the most recent pair for test inspection / runtime capture.
 *
 * The ring walk (lagfx_fifo_drain) remains stubbed pending R1.
 */

#include "fifo.h"
#include "protocol.h"
#include "state.h"
#include "opcodes.h"
#include "../common/log.h"

#include <string.h>

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

void lagfx_fifo_on_mmio_setter(lagfx_protocol_t *p,
                               uint64_t offset, uint32_t value) {
    if (!lagfx_protocol_is_valid(p)) {
        return;
    }

    p->last_setter_offset = (uint32_t)offset;
    p->last_setter_value  = value;
    p->setter_write_count += 1;

    /* Log at the candidate level until the runtime capture nails down
     * which offset carries which setter. The value is logged as both
     * hex (page number / length-in-bytes shape) and dec (byte offset
     * / write pointer shape) to aid disambiguation. */
    LAGFX_LOG("fifo: setter-candidate write off=0x%llx val=0x%08x (%u) #%llu",
              (unsigned long long)offset,
              value, value,
              (unsigned long long)p->setter_write_count);

    /* Attempt a drain regardless — if the write happened to be the
     * real doorbell, drain will work once ring geometry is known.
     * For now it's a no-op either way. */
    (void)lagfx_fifo_drain(p);
}

size_t lagfx_fifo_drain(lagfx_protocol_t *p) {
    if (!lagfx_protocol_is_valid(p)) {
        return 0;
    }

    /* TODO(R1): The ring-buffer GPA is not yet discovered. Per
     * re-followup-spec-gaps.md §1, the kext writes the ring geometry
     * via three MMIO setters (basePage, length, start) and then arms
     * the ring with a non-zero write to 0x1000. The doorbell is a
     * fourth setter (setFifoWritten) carrying a byte-offset write
     * pointer. Once runtime capture identifies which MMIO offsets
     * invoke which setters, this function should:
     *
     *   while (p->read_ptr != p->write_ptr) {
     *       uint8_t hdr_buf[LAGFX_CMD_HEADER_BYTES];
     *       p->dev->desc.shell.read_memory(
     *           p->dev->desc.shell.opaque,
     *           p->ring_base_gpa + p->read_ptr,
     *           LAGFX_CMD_HEADER_BYTES, hdr_buf);
     *       lagfx_cmd_header_t hdr;
     *       if (!lagfx_fifo_parse_header(hdr_buf,
     *                                    LAGFX_CMD_HEADER_BYTES,
     *                                    &hdr)) break;
     *       uint8_t cmd_buf[MAX_CMD_SIZE];
     *       p->dev->desc.shell.read_memory(
     *           p->dev->desc.shell.opaque,
     *           p->ring_base_gpa + p->read_ptr,
     *           hdr.length, cmd_buf);
     *       lagfx_protocol_dispatch_one(p, cmd_buf, hdr.length);
     *       p->read_ptr = (p->read_ptr + hdr.length) % p->ring_size;
     *   }
     */

    if (!p->ring_armed || p->ring_size == 0) {
        LAGFX_LOG("fifo_drain: ring not armed / no size — no-op (R1)");
        return 0;
    }

    LAGFX_LOG("fifo_drain: stub — ring GPA unresolved (R1 TODO)");
    return 0;
}

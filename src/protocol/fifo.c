/*
 * libapplegfx-vulkan — FIFO ring dequeue (Phase 1.A.2 skeleton)
 * src/protocol/fifo.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Skeleton. The real dequeue is blocked on R1 in
 * phase-1a2-decoder-plan.md §9 (ring GPA discovery). For now the
 * doorbell path records the stamp and calls fifo_drain, which logs
 * and returns. Tests exercise dispatch directly via
 * lagfx_protocol_dispatch_one.
 *
 * The header parser IS real — it's needed by both the stubbed ring
 * walk and by synthesized unit tests.
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

    uint8_t  opcode       = bytes[0];
    uint8_t  flags        = bytes[1];
    uint16_t total_length = read_le16(bytes + 2);
    uint32_t stamp        = read_le32(bytes + 4);
    uint32_t reserved     = read_le32(bytes + 8);
    uint16_t payload_size = read_le16(bytes + 12);
    uint16_t padding      = read_le16(bytes + 14);

    /* Sanity: length must be at least the header size. */
    if (total_length < LAGFX_CMD_HEADER_BYTES) {
        return false;
    }
    /* The caller may have a buffer shorter than total_length if only
     * the header was read; flag that via payload==NULL. */
    const uint8_t *payload = NULL;
    if (payload_size > 0 && len >= (size_t)total_length) {
        payload = bytes + LAGFX_CMD_HEADER_BYTES;
    }

    hdr_out->opcode       = opcode;
    hdr_out->flags        = flags;
    hdr_out->length       = total_length;
    hdr_out->stamp        = stamp;
    hdr_out->reserved     = reserved;
    hdr_out->payload_size = payload_size;
    hdr_out->padding      = padding;
    hdr_out->payload      = payload;

    return true;
}

void lagfx_fifo_on_doorbell(lagfx_protocol_t *p, uint32_t stamp) {
    if (!lagfx_protocol_is_valid(p)) {
        return;
    }

    p->last_doorbell_stamp = stamp;
    p->doorbell_writes += 1;

    LAGFX_LOG("doorbell: stamp=0x%08x (drain trigger; #%llu)",
              stamp, (unsigned long long)p->doorbell_writes);

    (void)lagfx_fifo_drain(p);
}

size_t lagfx_fifo_drain(lagfx_protocol_t *p) {
    if (!lagfx_protocol_is_valid(p)) {
        return 0;
    }

    /* TODO(R1): The ring-buffer GPA is not yet discovered. Per
     * phase-1a2-decoder-plan.md §3.2, the kext must publish the
     * ring address via some MMIO path we haven't identified yet
     * (candidates: 0x1008 write, 0x100c CONFIG, or an unknown
     * bootstrap channel). Until that's instrumented on real guest
     * traffic, there is nothing to read.
     *
     * When R1 resolves, the loop here should:
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
     *       // read payload if any
     *       uint8_t cmd_buf[MAX_CMD_SIZE];
     *       p->dev->desc.shell.read_memory(
     *           p->dev->desc.shell.opaque,
     *           p->ring_base_gpa + p->read_ptr,
     *           hdr.length, cmd_buf);
     *       lagfx_protocol_dispatch_one(p, cmd_buf, hdr.length);
     *       p->read_ptr = (p->read_ptr + hdr.length) % p->ring_size;
     *       if (hdr.stamp == p->last_doorbell_stamp) break;
     *   }
     */

    if (!p->ring_armed || p->ring_size == 0) {
        LAGFX_LOG("fifo_drain: ring not armed / no size — no-op (R1)");
        return 0;
    }

    LAGFX_LOG("fifo_drain: stub — ring GPA unresolved (R1 TODO)");
    return 0;
}

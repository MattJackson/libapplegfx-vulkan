/*
 * libapplegfx-vulkan — FIFO ring dequeue (Phase 1.A.2 skeleton)
 * src/protocol/fifo.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Skeleton only. The ring-buffer GPA mechanism is a known spec gap
 * (R1 in phase-1a2-decoder-plan.md §9): the kext must publish the
 * ring GPA through some MMIO path we haven't yet identified. Until
 * that's instrumented, fifo_drain() is a no-op that only bumps a
 * counter and returns. Tests drive dispatch via the direct
 * lagfx_protocol_dispatch_one() path instead.
 */

#ifndef LIBAPPLEGFX_PROTOCOL_FIFO_H
#define LIBAPPLEGFX_PROTOCOL_FIFO_H

#include "protocol.h"
#include "opcodes.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Called when the guest writes to LAGFX_REG_DOORBELL (0x101c).
 * Records the last-doorbell stamp and invokes fifo_drain. */
void lagfx_fifo_on_doorbell(lagfx_protocol_t *p, uint32_t stamp);

/* Walk the ring from read_ptr to (at most) the doorbell stamp.
 *
 * TODO(R1): Real implementation needs ring_base_gpa + ring_size
 * populated. Current implementation no-ops and returns 0 commands
 * processed. Once the kext's ring-setup MMIO path is identified,
 * this should:
 *   1. For each command in the ring:
 *      a. shell.read_memory(base_gpa + read_ptr, 16, header_buf)
 *      b. Parse header; if length > 16: read payload.
 *      c. Call lagfx_protocol_dispatch_one(header+payload).
 *      d. Advance read_ptr; wrap on ring_size boundary.
 *   2. Stop when read_ptr == write_ptr or stamp satisfied.
 */
size_t lagfx_fifo_drain(lagfx_protocol_t *p);

/* Parse a 16-byte command header from a raw byte stream. Returns
 * true on success (length >= 16 AND header.length >= 16). On success
 * hdr_out is populated (payload pointer is set to bytes + 16 if
 * payload_size > 0, else NULL). */
bool lagfx_fifo_parse_header(const uint8_t *bytes, size_t len,
                             lagfx_cmd_header_t *hdr_out);

#endif /* LIBAPPLEGFX_PROTOCOL_FIFO_H */

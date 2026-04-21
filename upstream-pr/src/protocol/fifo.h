/*
 * libapplegfx-vulkan — FIFO ring dequeue (Phase 1.A.2 skeleton)
 * src/protocol/fifo.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Skeleton only. The ring-buffer GPA is assembled at runtime from
 * three MMIO setter writes (setFifoBasePage:, setFifoLength:,
 * setFifoStart:) — see re-followup-spec-gaps.md §1 — and the doorbell
 * is a fourth setter (setFifoWritten:) that carries the advanced
 * write-pointer BYTE OFFSET, not a stamp. The exact MMIO offset →
 * setter mapping is currently unknown (all four setters live somewhere
 * in the range 0x1004..0x1034); runtime capture against a real guest
 * will disambiguate.
 *
 * Until then, lagfx_fifo_drain() is a no-op and the MMIO write path
 * just logs writes in the candidate range.
 */

#ifndef LIBAPPLEGFX_PROTOCOL_FIFO_H
#define LIBAPPLEGFX_PROTOCOL_FIFO_H

#include "protocol.h"
#include "opcodes.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Called when the guest writes to any MMIO offset in the setter
 * candidate range 0x1004..0x1034. The true identities (basePage /
 * length / start / written / other) are not yet known, so this
 * handler logs (offset, value) and treats the value as a
 * "doorbell-candidate write pointer" for the purpose of attempting a
 * drain. Tests use it via lagfx_protocol_last_setter_* accessors to
 * confirm the wiring. */
void lagfx_fifo_on_mmio_setter(lagfx_protocol_t *p,
                               uint64_t offset, uint32_t value);

/* Walk the ring from read_ptr up to the guest's advertised write
 * pointer.
 *
 * TODO(R1): Real implementation needs ring_base_gpa + ring_size
 * populated. Current implementation no-ops and returns 0 commands
 * processed. Once the runtime-capture plan in
 * re-followup-spec-gaps.md §1.5 nails down the offset mapping, this
 * should:
 *   1. While read_ptr != write_ptr:
 *      a. shell.read_memory(base_gpa + read_ptr, 12, header_buf)
 *      b. Parse 12-byte header; read the remaining length - 12 bytes
 *         of arg/tail data.
 *      c. Call lagfx_protocol_dispatch_one(hdr+payload).
 *      d. Advance read_ptr by hdr.length; wrap on ring_size.
 */
size_t lagfx_fifo_drain(lagfx_protocol_t *p);

/* Parse a 12-byte command header from a raw byte stream. Returns
 * true on success (input len >= 12 AND header.length >= 12).
 *
 * Fields populated on success:
 *   hdr_out->opcode, arg_count_8b, length, stamp — directly from the
 *     little-endian wire bytes.
 *   hdr_out->payload_size — derived as length - 12 (0 for a
 *     header-only command).
 *   hdr_out->payload — points at bytes + 12 iff the caller passed a
 *     buffer long enough to include the full command (len >= length);
 *     NULL otherwise (caller holds only the header). */
bool lagfx_fifo_parse_header(const uint8_t *bytes, size_t len,
                             lagfx_cmd_header_t *hdr_out);

#endif /* LIBAPPLEGFX_PROTOCOL_FIFO_H */

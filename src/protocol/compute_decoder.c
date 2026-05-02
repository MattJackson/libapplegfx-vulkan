/*
 * libapplegfx-vulkan — Compute-decoder dispatch entry point (M5 scaffold)
 * src/protocol/compute_decoder.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * IMPLEMENTATION STATUS (2026-05-01):
 *   🟡 Compute decoder: scaffolded, observation-only (M5)
 *   🟡 All compute opcodes are stubs (return 0, no Vulkan translation)
 *   ❌ No GPU work occurring (stubs don't translate to Vulkan)
 *   ❌ Channel 3 never opens (blocked by ABBA deadlock)
 *
 * RE references:
 *   - M5-air-translation-status.md: Vulkan translation status
 *   - PGDeserializerComputeDecoder: kext handler class
 *   - state-machines/compute-decoder-handlers.tsv: opcode table
 *
 * Implements `lagfx_compute_decoder_dispatch()` — looks the opcode up in
 * the table populated by compute_opcodes.c, prints the canonical
 * "[lagfx compute] op=0xNNN (name) — ack-only stub" trace, and forwards
 * to the descriptor's handler. Unknown opcodes are logged and absorbed.
 *
 * Last updated: 2026-05-01
 */

#include "compute_decoder.h"
#include "compute_opcodes.h"

#include "../common/log.h"

#include <stdio.h>

int lagfx_compute_decoder_dispatch(lagfx_protocol_t *p,
                                   uint32_t          opcode,
                                   const uint8_t    *payload,
                                   size_t            len) {
    const lagfx_compute_op_descriptor_t *d = lagfx_compute_op_lookup(opcode);
    if (!d) {
        fprintf(stderr,
                "[lagfx compute] op=0x%03x (Unknown) "
                "— absorbed (no descriptor; len=%zu)\n",
                (unsigned)(opcode & 0xffffu), len);
        return 0;
    }

    if (lagfx_compute_op_is_stub(opcode)) {
        fprintf(stderr,
                "[lagfx compute] op=0x%03x (%s) — ack-only stub\n",
                (unsigned)(d->opcode & 0xffffu), d->name);
    }

    if (d->default_handler == NULL) {
        return 0;
    }
    return d->default_handler(p, payload, len);
}

/*
 * libapplegfx-vulkan — Blit-decoder dispatch entry point (M5 scaffold)
 * src/protocol/blit_decoder.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * IMPLEMENTATION STATUS (2026-05-01):
 *   🟡 Blit decoder: scaffolded, observation-only (M5)
 *   🟡 All blit opcodes are stubs (return 0, no Vulkan translation)
 *   ❌ No GPU work occurring (stubs don't translate to Vulkan)
 *   ❌ Channel 3 never opens (blocked by ABBA deadlock)
 *
 * RE references:
 *   - M5-air-translation-status.md: Vulkan translation status
 *   - PGDeserializerBlitDecoder: kext handler class
 *   - state-machines/blit-decoder-handlers.tsv: opcode table
 *
 * Implements `lagfx_blit_decoder_dispatch()` — looks the opcode up in
 * the table populated by blit_opcodes.c, prints the canonical
 * "[lagfx blit] op=0xNNN (name) — ack-only stub" trace, and forwards
 * to the descriptor's handler. Unknown opcodes are logged and absorbed.
 *
 * Last updated: 2026-05-01
 */

#include "blit_decoder.h"
#include "blit_opcodes.h"

#include "../common/log.h"

#include <stdio.h>

int lagfx_blit_decoder_dispatch(lagfx_protocol_t *p,
                                uint32_t          opcode,
                                const uint8_t    *payload,
                                size_t            len) {
    const lagfx_blit_op_descriptor_t *d = lagfx_blit_op_lookup(opcode);
    if (!d) {
        fprintf(stderr,
                "[lagfx blit] op=0x%03x (Unknown) "
                "— absorbed (no descriptor; len=%zu)\n",
                (unsigned)(opcode & 0xffffu), len);
        return 0;
    }

    if (lagfx_blit_op_is_stub(opcode)) {
        fprintf(stderr,
                "[lagfx blit] op=0x%03x (%s) — ack-only stub\n",
                (unsigned)(d->opcode & 0xffffu), d->name);
    }

    if (d->default_handler == NULL) {
        return 0;
    }
    return d->default_handler(p, payload, len);
}

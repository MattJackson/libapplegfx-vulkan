/*
 * libapplegfx-vulkan — Render-decoder dispatch entry point (M5 scaffold)
 * src/protocol/render_decoder.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implements `lagfx_render_decoder_dispatch()` — looks the opcode up in
 * the table populated by render_opcodes.c, prints the canonical
 * "[lagfx render] op=0xNN (name) — ack-only stub" trace required by the
 * M5 brief, and forwards to the descriptor's handler. Unknown opcodes
 * are logged and absorbed (fail-open).
 */

#include "render_decoder.h"
#include "render_opcodes.h"

#include "../common/log.h"

#include <stdio.h>

int lagfx_render_decoder_dispatch(lagfx_protocol_t *p,
                                   uint32_t          opcode,
                                   const uint8_t    *payload,
                                   size_t            len) {
    /* `payload == NULL` is only valid when `len == 0`. We don't bail
     * — even a malformed (NULL, len>0) call lands in the ack-only
     * stub which ignores its inputs anyway, so during M5 bring-up we
     * keep the dispatcher fail-open and rely on the trace to surface
     * the issue. */
    
    /* Debug: log all render decoder calls for Stage 20 verification */
    LAGFX_TRACE("[lagfx render] dispatch called: opcode=0x%02x len=%zu",
            (unsigned)(opcode & 0xffu), len);
    
    const lagfx_render_op_descriptor_t *d = lagfx_render_op_lookup(opcode);
    if (!d) {
        /* Unknown / out-of-range opcode. Log unconditionally (this is
         * exactly the kind of event that should never go silent during
         * bring-up) and absorb. */
        LAGFX_WARN("[lagfx render] op=0x%02x (Unknown) "
                "— absorbed (no descriptor; len=%zu)",
                (unsigned)(opcode & 0xffu), len);
        return 0;
    }

    if (lagfx_render_op_is_stub(opcode)) {
        LAGFX_TRACE("[lagfx render] op=0x%02x (%s) — ack-only stub",
                (unsigned)(d->opcode & 0xffu), d->name);
    } else {
        LAGFX_TRACE("[lagfx render] op=0x%02x (%s) — real handler",
                (unsigned)(d->opcode & 0xffu), d->name);
    }

    if (d->default_handler == NULL) {
        /* Should not happen in the M5 scaffold — every entry has a
         * handler — but guard against future entries that go NULL
         * during a real-handler transition. */
        return 0;
    }
    int rc = d->default_handler(p, payload, len);
    LAGFX_TRACE("[lagfx render] opcode=0x%02x handler returned: %d",
            (unsigned)(opcode & 0xffu), rc);
    return rc;
}

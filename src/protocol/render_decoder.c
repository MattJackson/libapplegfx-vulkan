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
    const lagfx_render_op_descriptor_t *d = lagfx_render_op_lookup(opcode);
    if (!d) {
        /* Unknown / out-of-range opcode. Log unconditionally (this is
         * exactly the kind of event that should never go silent during
         * bring-up) and absorb. */
        fprintf(stderr,
                "[lagfx render] op=0x%02x (Unknown) "
                "— absorbed (no descriptor; len=%zu)\n",
                (unsigned)(opcode & 0xffu), len);
        return 0;
    }

    /* Required trace per the M5 brief: every dispatched opcode prints
     *   [lagfx render] op=0xNN (name) — ack-only stub
     * Print unconditionally during scaffolding so the operator can
     * confirm the dispatcher fired without flipping LAGFX_LOG. The
     * line will be removed (or routed through LAGFX_LOG) when real
     * handlers replace the stubs. */
    fprintf(stderr,
            "[lagfx render] op=0x%02x (%s) — ack-only stub\n",
            (unsigned)(d->opcode & 0xffu), d->name);

    if (d->default_handler == NULL) {
        /* Should not happen in the M5 scaffold — every entry has a
         * handler — but guard against future entries that go NULL
         * during a real-handler transition. */
        return 0;
    }
    return d->default_handler(p, payload, len);
}

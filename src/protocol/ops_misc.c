/*
 * libapplegfx-vulkan — misc opcode handlers (NOP, Debug)
 * src/protocol/ops_misc.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Phase 1.A.2 "real" handlers. Both are simple enough to ship
 * end-to-end and serve as bring-up vehicles:
 *
 *   - CmdNOP (0x0e)  — accept, return success. Per
 *     command-buffer-format.md §3 it has no payload and no
 *     response; we honor completion flags anyway so tests that
 *     set COMPLETION_EXPECTED get a fence/IRQ.
 *
 *   - CmdDebug (0x0d) — log the debug payload, return success.
 *     Useful bring-up / trace vehicle; dylib exposes a matching
 *     -[PGFIFO CmdDebug:] handler (see §3 table).
 *
 * The actual completion stamp + interrupt are driven by the
 * dispatcher in protocol.c, not by the handler — handlers only
 * need to do opcode-specific work.
 */

#include "opcodes.h"
#include "protocol.h"
#include "state.h"
#include "../common/log.h"

#include <stdio.h>

lagfx_handler_status_t lagfx_op_nop(lagfx_protocol_t *p,
                                    const lagfx_cmd_header_t *hdr) {
    (void)p;
    if (!hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    LAGFX_LOG("CmdNOP: stamp=0x%08x flags=0x%02x",
              hdr->stamp, hdr->flags);
    return LAGFX_HANDLER_OK;
}

/* Debug commands carry an opaque payload (debugCode + bytes, per
 * the spec table). We log the first 16 bytes of payload to aid
 * bring-up without spamming multi-KB dumps. */
lagfx_handler_status_t lagfx_op_debug(lagfx_protocol_t *p,
                                      const lagfx_cmd_header_t *hdr) {
    (void)p;
    if (!hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }

    if (hdr->payload && hdr->payload_size > 0) {
        /* Hex-dump up to 16 bytes. */
        char hex[3 * 16 + 1] = {0};
        size_t n = hdr->payload_size < 16 ? hdr->payload_size : 16;
        for (size_t i = 0; i < n; ++i) {
            snprintf(hex + (i * 3), sizeof(hex) - (i * 3),
                     "%02x ", hdr->payload[i]);
        }
        LAGFX_LOG("CmdDebug: stamp=0x%08x flags=0x%02x "
                  "payload_size=%u bytes=[%s%s]",
                  hdr->stamp, hdr->flags,
                  (unsigned)hdr->payload_size,
                  hex,
                  hdr->payload_size > 16 ? "..." : "");
    } else {
        LAGFX_LOG("CmdDebug: stamp=0x%08x flags=0x%02x (no payload)",
                  hdr->stamp, hdr->flags);
    }
    return LAGFX_HANDLER_OK;
}

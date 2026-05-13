/*
 * libapplegfx-vulkan — queue-domain opcode handlers
 * src/protocol/ops_queue.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Phase 1.A.2 real handlers for child-FIFO lifecycle:
 *
 *   CmdDefineChildFIFO  (0x04) P0 — implemented. Per
 *     re-followup-spec-gaps.md §3 (HIGH, 85%), the payload is just a
 *     4-byte u32 fifoID. The child ring geometry (base, length, start)
 *     is pre-registered out-of-band via MMIO setter writes, NOT via
 *     this command.
 *   CmdDeleteChildFIFO  (0x05) P0 — implemented. 4-byte u32 fifoID.
 */

#include "opcodes.h"
#include "protocol.h"
#include "state.h"
#include "../common/le.h"
#include "../common/log.h"

#include <string.h>

/* Flag set when CmdDefineChildFIFO is called, signaling device creation complete.
 * Used by ops_display.c and ops_display_vchan.c to wait for WindowServer display init before firing online event.
 *
 * Canonical per-protocol storage lives at p->cmd_define_fifo_called
 * (cleared in lagfx_protocol_reset). The file-scope static below is a
 * legacy mirror because the public setter zero-arg
 * lagfx_ops_queue_set_cmddefine_called() is called from
 * ops_display_vchan.c without a protocol pointer in scope. */
static bool g_cmd_define_fifo_called = false;

bool lagfx_ops_queue_cmddefine_called(void) {
    return g_cmd_define_fifo_called;
}

void lagfx_ops_queue_reset(void) {
    g_cmd_define_fifo_called = false;
}

/* Setter function for display vchan handler to signal device creation complete */
void lagfx_ops_queue_set_cmddefine_called(void) {
    g_cmd_define_fifo_called = true;
}

/* ===========================================================================
 * CmdDefineChildFIFO (0x04) — P0
 *
 * Request layout (re-followup-spec-gaps.md §3.3):
 *   payload[0..3] u32 fifoID
 *
 * The dylib handler `-[PGFIFO CmdDefineChildFIFO:...]` at dylib vaddr
 * 0x7ffb0dfdaddf reads ONLY the first 4 bytes and calls a single-arg
 * selector on _PGDevice with the fifoID (disasm lines 70595–70600).
 * Per-FIFO ring geometry is already in the device state via the same
 * MMIO setter path used for the root ring.
 *
 * We register fifoID → slot in p->fifos. Duplicates re-use existing
 * slot. Buffer VA / size are left zeroed (set separately when the
 * ring-geometry setters get disambiguated by runtime capture).
 * =========================================================================== */

lagfx_handler_status_t lagfx_op_define_child_fifo(lagfx_protocol_t *p,
                                                  const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    if (!hdr->payload || hdr->payload_size < 4) {
        LAGFX_WARN("CmdDefineChildFIFO: payload missing or too small "
                   "(size=%u)", (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    LAGFX_WARN("CmdDefineChildFIFO: payload_size=%u first32_bytes:",
               (unsigned)hdr->payload_size);
    {
        unsigned dump_n = hdr->payload_size > 64 ? 64 : hdr->payload_size;
        char hex[200];
        for (unsigned i = 0; i < dump_n; i++) {
            snprintf(hex + i * 3, 4, "%02x ", hdr->payload[i]);
        }
        LAGFX_WARN("  [%s]", hex);
    }

    uint32_t fifo_id = lagfx_le32(hdr->payload + 0);

    lagfx_childfifo_entry_t *entry = lagfx_protocol_find_fifo(p, fifo_id);
    if (entry) {
        LAGFX_WARN("CmdDefineChildFIFO: duplicate fifoID=%u "
                   "(re-using slot)", fifo_id);
    } else {
        entry = lagfx_protocol_alloc_fifo_slot(p);
        if (!entry) {
            LAGFX_WARN("CmdDefineChildFIFO: fifo table full (max=%u)",
                       LAGFX_MAX_CHILDFIFOS);
            return LAGFX_HANDLER_ERR_STATE;
        }
    }

    entry->id        = fifo_id;
    entry->buffer_va = 0;
    entry->size      = 0;
    entry->synced    = false;
    entry->live      = true;

    /* Signal that device creation is complete - WindowServer has started display init.
     * Write the canonical per-protocol flag and mirror to the file-scope
     * static for the legacy zero-arg accessor. */
    p->cmd_define_fifo_called = true;
    g_cmd_define_fifo_called  = true;
    LAGFX_LOG("CmdDefineChildFIFO: fifoID=%u stamp=0x%08x (ring geometry "
              "registered via MMIO setters, not payload)",
              fifo_id, hdr->stamp);
    return LAGFX_HANDLER_OK;
}

/* ===========================================================================
 * CmdDeleteChildFIFO (0x05) — P0
 *
 * Request layout: payload[0..3] u32 fifoID. Symmetric to DefineChildFIFO.
 * Errors if fifoID not found.
 * =========================================================================== */

lagfx_handler_status_t lagfx_op_delete_child_fifo(lagfx_protocol_t *p,
                                                  const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    if (!hdr->payload || hdr->payload_size < 4) {
        LAGFX_WARN("CmdDeleteChildFIFO: payload missing or too small "
                   "(size=%u)", (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint32_t fifo_id = lagfx_le32(hdr->payload + 0);

    lagfx_childfifo_entry_t *entry = lagfx_protocol_find_fifo(p, fifo_id);
    if (!entry) {
        LAGFX_WARN("CmdDeleteChildFIFO: fifoID=%u not found", fifo_id);
        return LAGFX_HANDLER_ERR_STATE;
    }

    LAGFX_LOG("CmdDeleteChildFIFO: fifoID=%u stamp=0x%08x",
              fifo_id, hdr->stamp);

    memset(entry, 0, sizeof(*entry));
    entry->live = false;
    return LAGFX_HANDLER_OK;
}

/*
 * libapplegfx-vulkan — queue-domain opcode stubs
 * src/protocol/ops_queue.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Phase 1.A.2 scaffold. Child-FIFO (command-queue) lifecycle
 * handlers. Next agent implements.
 *
 * Priorities from phase-1a2-decoder-plan.md §4:
 *
 *   CmdDefineChildFIFO  (0x04) P0 — MED confidence on arg layout (R9)
 *   CmdDeleteChildFIFO  (0x05) P0 — pair with above
 */

#include "opcodes.h"
#include "protocol.h"
#include "state.h"
#include "../common/log.h"

/* TODO(Phase-1.A.3): parse {u32 fifoID, u64 bufferVA, u32 size}
 * (layout R9 — MED confidence; log raw arg bytes on first few
 * invocations and verify). Record in p->fifos. */
lagfx_handler_status_t lagfx_op_define_child_fifo(lagfx_protocol_t *p,
                                                  const lagfx_cmd_header_t *hdr) {
    (void)p; (void)hdr;
    LAGFX_LOG("CmdDefineChildFIFO: TODO(P0) stamp=0x%08x",
              hdr ? hdr->stamp : 0u);
    return LAGFX_HANDLER_OK;
}

/* TODO(Phase-1.A.3): parse {u32 fifoID}; mark entry !live. */
lagfx_handler_status_t lagfx_op_delete_child_fifo(lagfx_protocol_t *p,
                                                  const lagfx_cmd_header_t *hdr) {
    (void)p; (void)hdr;
    LAGFX_LOG("CmdDeleteChildFIFO: TODO(P0) stamp=0x%08x",
              hdr ? hdr->stamp : 0u);
    return LAGFX_HANDLER_OK;
}

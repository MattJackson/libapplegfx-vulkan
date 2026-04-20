/*
 * libapplegfx-vulkan — cmdbuf-domain opcode stubs
 * src/protocol/ops_cmdbuf.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Phase 1.A.2 scaffold. Command-buffer submission path handlers.
 * Next agent implements.
 *
 * Priorities from phase-1a2-decoder-plan.md §4:
 *
 *   CmdSynchronizeResources (0x22) P0 — critical; empty-list is
 *     the most likely opcode for `[cmdbuf commit]` with empty
 *     cmdbuf (R2 — could also be ExecIndirect2).
 *   CmdExecIndirect2        (0x20) P1 — alternate path for same
 *     metal-no-op scenario (R2).
 */

#include "opcodes.h"
#include "protocol.h"
#include "state.h"
#include "../common/log.h"

/* TODO(Phase-1.A.3): parse {u32 count, u32 resource_ids[count]};
 * for empty list (count=0) this is a pure completion-stamp vehicle
 * — dispatcher already writes fence + raises IRQ when
 * COMPLETION_EXPECTED set. Real implementation needs to traverse
 * resources and resolve barriers. */
lagfx_handler_status_t lagfx_op_synchronize_resources(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    (void)p; (void)hdr;
    LAGFX_LOG("CmdSynchronizeResources: TODO(P0) stamp=0x%08x",
              hdr ? hdr->stamp : 0u);
    return LAGFX_HANDLER_OK;
}

/* TODO(Phase-1.A.3): parse {u32 cmdBufCount, CommandBuffer buffers[],
 * u32 resourceCount, ResourceRef resources[]}. For empty lists this
 * is also a pure completion vehicle. Render/compute execution is
 * explicitly out of 1.A.2 scope (see §1 "Out of scope"). */
lagfx_handler_status_t lagfx_op_exec_indirect2(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    (void)p; (void)hdr;
    LAGFX_LOG("CmdExecIndirect2: TODO(P1) stamp=0x%08x",
              hdr ? hdr->stamp : 0u);
    return LAGFX_HANDLER_OK;
}

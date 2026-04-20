/*
 * libapplegfx-vulkan — cmdbuf-domain opcode handlers
 * src/protocol/ops_cmdbuf.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Phase 1.A.2 real handlers for the command-buffer submission path:
 *
 *   CmdSynchronizeResources (0x22) P0 — implemented. Per
 *     re-followup-spec-gaps.md §4 (HIGH, 90%): payload is
 *     {u32 taskID, u32 count, u32 resource_ids[count]}, total 8+4N
 *     bytes. count=0 is the completion path for `[cmdbuf commit]` on
 *     an empty cmdbuf (plan §6.2).
 *
 *   CmdExecIndirect2 (0x20) P1 — stubbed but documents the alternate
 *     empty-cmdbuf completion path (R2). Empty list completes
 *     immediately (fail-open via dispatcher).
 */

#include "opcodes.h"
#include "protocol.h"
#include "state.h"
#include "../common/log.h"

static inline uint32_t lagfx_le32(const uint8_t *b) {
    return (uint32_t)b[0]
         | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16)
         | ((uint32_t)b[3] << 24);
}

/* ===========================================================================
 * CmdSynchronizeResources (0x22) — P0
 *
 * Request layout (re-followup-spec-gaps.md §4.3, HIGH 90%):
 *   payload[0..3]            u32 taskID
 *   payload[4..7]            u32 count
 *   payload[8..(8 + 4*count)] u32 resource_ids[count]
 *
 * Total payload bytes: 8 + 4*count. Minimum: 8 bytes (count=0, empty
 * list — the `[cmdbuf commit]` on empty cmdbuf path).
 *
 * Semantics (scaffold):
 *   - count=0: instant completion (dispatcher signals stamp).
 *   - count>0: for each ID, scan the fifo table and mark synced=true
 *     if present. This is a Phase 1.A.2 placeholder; full per-resource
 *     barrier tracking lands in Phase 3. The taskID is validated
 *     against the task table — unknown taskID is a warning but does
 *     not fail (fail-open).
 *
 * Overflow guard: we require payload_size >= 8 + 4*count, mirroring the
 * dylib's `cmpq %r13, %r15; ja fault` check at disasm line 71267–71268.
 * =========================================================================== */

lagfx_handler_status_t lagfx_op_synchronize_resources(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    if (!hdr->payload || hdr->payload_size < 8) {
        LAGFX_WARN("CmdSynchronizeResources: payload missing or too small "
                   "(size=%u, need >= 8)", (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint32_t task_id = lagfx_le32(hdr->payload + 0);
    uint32_t count   = lagfx_le32(hdr->payload + 4);

    /* Overflow-safe size check: 8 + 4*count must fit within payload_size. */
    if (count > ((uint32_t)hdr->payload_size - 8u) / 4u) {
        LAGFX_WARN("CmdSynchronizeResources: count=%u exceeds payload "
                   "(size=%u)", count, (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    /* Empty list — pure completion vehicle. Matches metal-no-op. */
    if (count == 0) {
        LAGFX_LOG("CmdSynchronizeResources: taskID=%u count=0 "
                  "(empty-list completion) stamp=0x%08x",
                  task_id, hdr->stamp);
        return LAGFX_HANDLER_OK;
    }

    /* Non-empty: validate taskID against the task table. Unknown taskID
     * is logged but not fatal — fail-open per spec §6. */
    lagfx_task_entry_t *task = lagfx_protocol_find_task(p, task_id);
    if (!task) {
        LAGFX_WARN("CmdSynchronizeResources: taskID=%u not found "
                   "(continuing fail-open)", task_id);
    }

    /* Walk the id list and mark matching child-FIFO entries synced.
     * Phase 1.A.2 uses child-FIFO IDs as a crude stand-in for "resource
     * IDs" — real resources include textures/buffers/heaps which we do
     * not yet track; full resource table lands in Phase 2/3. */
    unsigned matched = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t rid = lagfx_le32(hdr->payload + 8u + 4u * i);
        lagfx_childfifo_entry_t *fifo = lagfx_protocol_find_fifo(p, rid);
        if (fifo) {
            fifo->synced = true;
            matched++;
        }
    }

    LAGFX_LOG("CmdSynchronizeResources: taskID=%u count=%u matched=%u "
              "stamp=0x%08x",
              task_id, count, matched, hdr->stamp);
    return LAGFX_HANDLER_OK;
}

/* ===========================================================================
 * CmdExecIndirect2 (0x20) — P1 (deferred)
 *
 * Alternate empty-cmdbuf completion path per re-followup-spec-gaps.md
 * R2 (plan §9). Empty-list case should complete immediately, matching
 * the observed pattern for `[cmdbuf commit]` on an empty command
 * buffer. Full render/compute execution is out of 1.A.2 scope (plan §1
 * "Out of scope").
 *
 * Layout (plan §4.2, MED confidence):
 *   u32 cmdBufCount, CommandBuffer buffers[],
 *   u32 resourceCount, ResourceRef resources[]
 *
 * Until the layout is confirmed we treat ANY length as instant
 * success — the dispatcher's completion stamp is sufficient to keep
 * the guest unblocked.
 * =========================================================================== */

lagfx_handler_status_t lagfx_op_exec_indirect2(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    (void)p;
    LAGFX_LOG("CmdExecIndirect2: TODO(P1) stamp=0x%08x payload_size=%u "
              "(empty-list completion path — layout unconfirmed)",
              hdr ? hdr->stamp : 0u,
              hdr ? (unsigned)hdr->payload_size : 0u);
    return LAGFX_HANDLER_OK;
}

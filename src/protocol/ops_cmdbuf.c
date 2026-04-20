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
 *   CmdExecIndirect2 (0x20) P1 — scaffolded. Per
 *     re-followup-spec-gaps.md R2 and phase-1a2-decoder-plan.md §6.2,
 *     empty-list case (count=0) is the alternate completion path for
 *     `[cmdbuf commit]` on an empty cmdbuf; count>0 path is scaffolded
 *     by walking the list and marking implicated child-FIFOs synced
 *     (mirroring CmdSynchronizeResources count>0 behaviour). Real
 *     indirect-exec dispatch is Phase 3.
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
 * CmdExecIndirect2 (0x20) — P1 (Phase 1.A.2, scaffolded)
 *
 * Alternate empty-cmdbuf completion path per re-followup-spec-gaps.md
 * R2 (plan §9, scope-audit-1080p30fps.md §Phase 2.A). The
 * `[cmdbuf commit]` path for an empty command buffer may land on 0x22
 * (CmdSynchronizeResources) OR 0x20 (CmdExecIndirect2) with an empty
 * buffer list. This handler mirrors the CmdSynchronizeResources
 * count-handling pattern so both paths look identical to the guest.
 *
 * Request layout (PARTIAL confidence — re-followup-spec-gaps.md does
 * NOT decode this opcode). Conservative guess, aligned with
 * command-buffer-format.md §3 (MED-HIGH):
 *
 *     payload[0..3]             u32 taskID
 *     payload[4..7]             u32 count            (# of indirect cmd ids)
 *     payload[8..(8 + 4*count)] u32 indirect_cmd_id[count]
 *
 * Minimum payload: 8 bytes (count=0, empty-list completion). Matches
 * the dispatcher's min_payload=0 gate (we tolerate shorter payloads by
 * treating them as count=0 — the real dylib handler almost certainly
 * has its own min check that we don't yet know).
 *
 * Semantics:
 *   - count=0 (or payload < 8 bytes): instant completion — the
 *     metal-no-op alternate path.
 *   - count>0: SCAFFOLDED — walk the ID list, mark matching child-FIFO
 *     entries synced (same pattern as CmdSynchronizeResources). This is
 *     placeholder behaviour; real indirect-exec dispatch (driving GPU
 *     work through nested command buffers) is Phase 3.
 *
 * Completion is unconditional via the dispatcher.
 * =========================================================================== */

lagfx_handler_status_t lagfx_op_exec_indirect2(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }

    /* No payload or < 8 bytes: treat as empty-list completion (the
     * fallback metal-no-op path an implementation might choose instead
     * of CmdSynchronizeResources). Spec descriptor sets min_payload=0,
     * so this branch is valid. */
    if (!hdr->payload || hdr->payload_size < 8) {
        LAGFX_LOG("CmdExecIndirect2: stamp=0x%08x payload_size=%u "
                  "(empty-list completion — metal-no-op alternate path)",
                  hdr->stamp, (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_OK;
    }

    uint32_t task_id = lagfx_le32(hdr->payload + 0);
    uint32_t count   = lagfx_le32(hdr->payload + 4);

    /* count=0: explicit empty list. Complete cleanly. */
    if (count == 0) {
        LAGFX_LOG("CmdExecIndirect2: taskID=%u count=0 "
                  "(empty-list completion) stamp=0x%08x",
                  task_id, hdr->stamp);
        return LAGFX_HANDLER_OK;
    }

    /* Overflow-safe size check: 8 + 4*count must fit in payload_size.
     * If the guest supplied a count>0 but the payload is too short,
     * refuse — but the dispatcher still signals the stamp. */
    if (count > ((uint32_t)hdr->payload_size - 8u) / 4u) {
        LAGFX_WARN("CmdExecIndirect2: count=%u exceeds payload "
                   "(size=%u)", count, (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    /* Non-empty: fail-open on unknown task (layout guess; real RE may
     * show a different first-field encoding — see re-followup spec gap
     * flag above). */
    lagfx_task_entry_t *task = lagfx_protocol_find_task(p, task_id);
    if (!task) {
        LAGFX_WARN("CmdExecIndirect2: taskID=%u not found "
                   "(continuing fail-open)", task_id);
    }

    /* count>0 path is scaffolded; real indirect-exec dispatch is Phase 3.
     * For now we mirror CmdSynchronizeResources behaviour: walk the ID
     * list, mark any matching child-FIFO entries synced so tests can
     * observe that the handler actually reached those IDs. */
    unsigned matched = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t rid = lagfx_le32(hdr->payload + 8u + 4u * i);
        lagfx_childfifo_entry_t *fifo = lagfx_protocol_find_fifo(p, rid);
        if (fifo) {
            fifo->synced = true;
            matched++;
        }
    }

    LAGFX_LOG("CmdExecIndirect2: taskID=%u count=%u matched=%u "
              "stamp=0x%08x (scaffolded — real exec dispatch is Phase 3)",
              task_id, count, matched, hdr->stamp);
    return LAGFX_HANDLER_OK;
}

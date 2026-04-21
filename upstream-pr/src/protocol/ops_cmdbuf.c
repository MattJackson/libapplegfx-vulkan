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
#include "../device.h"
#include "../vulkan/command.h"

static inline uint32_t lagfx_le32(const uint8_t *b) {
    return (uint32_t)b[0]
         | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16)
         | ((uint32_t)b[3] << 24);
}

/* Phase 1 end-to-end glue: the empty-cmdbuf completion paths (0x22 and
 * 0x20 with count=0) represent the guest saying "commit this cmdbuf with
 * no GPU work". On real hardware Apple still drives the GPU's submission
 * pipeline — fence, wait, completion interrupt. We mirror that by driving
 * a fence-gated VkQueueSubmit of an empty command buffer via the
 * Phase 1.B.2 helper lagfx_vk_submit_empty().
 *
 * This is additive: the decoder's own completion path (stamp writeback +
 * shell IRQ) already ran by the time the handler returns. The vk submit
 * exercises the Vulkan queue round-trip so we can observe an end-to-end
 * success even though no rendering occurred. The call is guarded so:
 *
 *   - Tests that pass NULL device (pre-Phase-1.B fixtures) skip silently.
 *   - Builds without Vulkan (LAGFX_HAVE_VULKAN unset) land in the
 *     command.c no-op stub, which returns LAGFX_OK.
 *   - Runs where the Vulkan state exists but didn't initialise (Darwin
 *     dev host with no loadable ICD) return LAGFX_ERR_VULKAN_INIT from
 *     inside lagfx_vk_submit_empty; we log and carry on — the decoder's
 *     own completion path is correctness-critical, not this.
 */
static void lagfx_cmdbuf_commit_empty_vk_submit(lagfx_protocol_t *p,
                                                uint16_t opcode,
                                                uint32_t stamp) {
    if (!p || !p->dev) {
        /* Test fixture path — no device attached. Silent skip. */
        return;
    }
    struct lagfx_vk_state *vk = p->dev->vk;
    if (!vk) {
        /* Should not happen post-1.B (device_new always populates vk)
         * but be defensive. */
        return;
    }
    lagfx_status_t vst = lagfx_vk_submit_empty(vk);
    if (vst != LAGFX_OK) {
        /* Not fatal for the guest-visible command completion. On Darwin
         * dev hosts with no ICD this will fire on every empty-cmdbuf
         * commit; LAGFX_LOG keeps it at the usual verbosity. */
        LAGFX_LOG("cmdbuf_commit_empty: vk submit skipped/failed "
                  "(opcode=0x%04x stamp=0x%08x status=%d)",
                  opcode, stamp, (int)vst);
    }
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

    /* Empty list — pure completion vehicle. Matches metal-no-op. Drive
     * the Vulkan queue round-trip so the guest's empty-cmdbuf commit
     * becomes an end-to-end VkSubmit on the host. See helper comment
     * above for the guard rationale. */
    if (count == 0) {
        LAGFX_LOG("CmdSynchronizeResources: taskID=%u count=0 "
                  "(empty-list completion) stamp=0x%08x",
                  task_id, hdr->stamp);
        lagfx_cmdbuf_commit_empty_vk_submit(p, LAGFX_OP_SYNCHRONIZE_RESOURCES,
                                            hdr->stamp);
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
 * CmdExecIndirect2 (0x20) — P1 partial (Phase 3.A scaffold)
 *
 * PARTIAL confidence. Per command-buffer-format.md §12 ("hardest opcode
 * encountered") and phase-3-metal-vulkan-plan.md §R3.6, the inner
 * command-stream layout inside this opcode's nested buffer is the
 * single biggest unknown in Phase 3. A 2-day runtime-capture RE spike
 * at Phase 3.A day 1–2 is required to confirm the inner-opcode numeric
 * IDs, the per-entry header layout, and the payload encodings.
 *
 * Outer request layout (guess, aligned with command-buffer-format.md
 * §3 MED-HIGH for the 0x20 wrapper):
 *
 *     payload[0..3]             u32 taskID
 *     payload[4..7]             u32 count            (# of inner entries)
 *     payload[8..]              inner entries (see below)
 *
 * Inner-entry layout (PARTIAL guess — per phase-3-metal-vulkan-plan.md
 * §3.A + common Apple PVG conventions, each inner entry is assumed to
 * carry an 8-byte header of {u32 inner_opcode, u32 inner_length}
 * followed by inner_length - 8 bytes of inner payload). Runtime RE
 * capture required to confirm. If the real wire uses a different
 * header (e.g., packed u16 opcode + u16 length, or 4-byte headers),
 * only this decode block needs to change — the lagfx_process_inner()
 * dispatch is stable.
 *
 * Semantics:
 *   - count=0 (or payload < 8 bytes): instant completion — the
 *     metal-no-op alternate empty-cmdbuf path (pre-Phase-3 behaviour
 *     preserved).
 *   - count>0: walk each inner entry, decode its 8-byte inner header,
 *     invoke lagfx_process_inner() which dispatches to per-inner-opcode
 *     stub handlers. Each stub logs + bumps counters; Phase 3.A.2 will
 *     replace the log-only stubs with real Vulkan record operations
 *     (vkCmdBindShadersEXT, vkCmdDraw, etc.) against a VkCommandBuffer.
 *
 * The outer stamp is signalled unconditionally by the dispatcher.
 * =========================================================================== */

/* Inner-opcode stub handlers — minimal work + counter bumps + log.
 * All take (p, payload, payload_len) where payload is past the 8-byte
 * inner header. Return LAGFX_HANDLER_OK on success or
 * LAGFX_HANDLER_ERR_SIZE on underflow. None abort the outer dispatch
 * — the caller continues walking the inner stream on any single-entry
 * failure (fail-open per command-buffer-format.md §6). */

static lagfx_handler_status_t lagfx_inner_bind_pipeline(
    lagfx_protocol_t *p, const uint8_t *payload, size_t payload_len) {
    if (payload_len < 4) {
        LAGFX_WARN("inner BIND_PIPELINE: payload too small (%zu < 4)",
                   payload_len);
        return LAGFX_HANDLER_ERR_SIZE;
    }
    uint32_t pipeline_handle = lagfx_le32(payload);
    p->inner_opcodes_bind_pipeline++;

    /* Update every live display's last_pipeline. Phase 3.A.2 will bind
     * against an actual display-scoped VkCommandBuffer; for now we just
     * observe the handle so tests can see the dispatch landed. */
    for (unsigned i = 0; i < LAGFX_PROTO_MAX_DISPLAYS; ++i) {
        if (p->displays[i].live) {
            p->displays[i].last_pipeline = pipeline_handle;
        }
    }

    LAGFX_LOG("inner BIND_PIPELINE: handle=0x%08x", pipeline_handle);
    return LAGFX_HANDLER_OK;
}

static lagfx_handler_status_t lagfx_inner_bind_vertex_buffer(
    lagfx_protocol_t *p, const uint8_t *payload, size_t payload_len) {
    if (payload_len < 12) {
        LAGFX_WARN("inner BIND_VERTEX_BUFFER: payload too small (%zu < 12)",
                   payload_len);
        return LAGFX_HANDLER_ERR_SIZE;
    }
    uint32_t buffer_handle = lagfx_le32(payload);
    /* offset occupies the next 8 bytes (u64 LE). */
    uint64_t offset =
        (uint64_t)lagfx_le32(payload + 4) |
        ((uint64_t)lagfx_le32(payload + 8) << 32);
    p->inner_opcodes_bind_vertex_buffer++;
    LAGFX_LOG("inner BIND_VERTEX_BUFFER: handle=0x%08x offset=0x%llx",
              buffer_handle, (unsigned long long)offset);
    return LAGFX_HANDLER_OK;
}

static lagfx_handler_status_t lagfx_inner_bind_fragment_resource(
    lagfx_protocol_t *p, const uint8_t *payload, size_t payload_len) {
    if (payload_len < 4) {
        LAGFX_WARN("inner BIND_FRAGMENT_RESOURCE: payload too small "
                   "(%zu < 4)", payload_len);
        return LAGFX_HANDLER_ERR_SIZE;
    }
    uint32_t resource_handle = lagfx_le32(payload);
    p->inner_opcodes_bind_fragment_resource++;
    LAGFX_LOG("inner BIND_FRAGMENT_RESOURCE: handle=0x%08x",
              resource_handle);
    return LAGFX_HANDLER_OK;
}

static lagfx_handler_status_t lagfx_inner_set_render_target(
    lagfx_protocol_t *p, const uint8_t *payload, size_t payload_len) {
    if (payload_len < 4) {
        LAGFX_WARN("inner SET_RENDER_TARGET: payload too small (%zu < 4)",
                   payload_len);
        return LAGFX_HANDLER_ERR_SIZE;
    }
    uint32_t target_handle = lagfx_le32(payload);
    p->inner_opcodes_set_render_target++;
    LAGFX_LOG("inner SET_RENDER_TARGET: handle=0x%08x",
              target_handle);
    return LAGFX_HANDLER_OK;
}

static lagfx_handler_status_t lagfx_inner_draw(
    lagfx_protocol_t *p, const uint8_t *payload, size_t payload_len) {
    if (payload_len < 8) {
        LAGFX_WARN("inner DRAW: payload too small (%zu < 8)",
                   payload_len);
        return LAGFX_HANDLER_ERR_SIZE;
    }
    uint32_t vertex_count   = lagfx_le32(payload);
    uint32_t instance_count = lagfx_le32(payload + 4);
    p->inner_opcodes_draw++;

    /* TODO(Phase 3.A.2): Record vkCmdBindPipeline + vkCmdDraw against
     * the display-scoped VkCommandBuffer here. Need pipeline-state
     * plumbing from last_pipeline + descriptor sets accumulated from
     * BIND_VERTEX_BUFFER / BIND_FRAGMENT_RESOURCE. For now, log-only —
     * per phase-3-metal-vulkan-plan.md §3.A this is the Phase 3.A.2
     * entry point once the RE spike confirms inner-opcode semantics. */
    LAGFX_LOG("inner DRAW: vertex_count=%u instance_count=%u "
              "(Phase 3.A.2 will record vkCmdDraw here)",
              vertex_count, instance_count);
    return LAGFX_HANDLER_OK;
}

static lagfx_handler_status_t lagfx_inner_set_viewport(
    lagfx_protocol_t *p, const uint8_t *payload, size_t payload_len) {
    if (payload_len < 16) {
        LAGFX_WARN("inner SET_VIEWPORT: payload too small (%zu < 16)",
                   payload_len);
        return LAGFX_HANDLER_ERR_SIZE;
    }
    uint32_t x = lagfx_le32(payload);
    uint32_t y = lagfx_le32(payload + 4);
    uint32_t w = lagfx_le32(payload + 8);
    uint32_t h = lagfx_le32(payload + 12);
    p->inner_opcodes_set_viewport++;
    LAGFX_LOG("inner SET_VIEWPORT: rect=(%u,%u,%u,%u)", x, y, w, h);
    return LAGFX_HANDLER_OK;
}

static lagfx_handler_status_t lagfx_inner_unknown(
    lagfx_protocol_t *p, uint32_t inner_opcode,
    const uint8_t *payload, size_t payload_len) {
    p->inner_opcodes_unknown++;

    /* Log a short hex dump of up to the first 16 bytes so runtime RE
     * capture has something to match against. */
    char hex[3 * 16 + 1];
    size_t dump = payload_len < 16 ? payload_len : 16;
    for (size_t i = 0; i < dump; ++i) {
        snprintf(hex + i * 3, 4, "%02x ", payload[i]);
    }
    hex[dump ? dump * 3 - 1 : 0] = '\0';
    LAGFX_LOG("inner UNKNOWN: opcode=0x%08x payload_len=%zu hex=[%s%s]",
              inner_opcode, payload_len, hex,
              payload_len > 16 ? " ..." : "");
    return LAGFX_HANDLER_OK;
}

/* Dispatch one inner-opcode entry. payload points past the 8-byte
 * inner header; payload_len is inner_length - 8. */
static lagfx_handler_status_t lagfx_process_inner(
    lagfx_protocol_t *p, uint32_t inner_opcode,
    const uint8_t *payload, size_t payload_len) {
    p->inner_opcodes_processed++;
    switch ((lagfx_inner_opcode_t)inner_opcode) {
    case LAGFX_INNER_BIND_PIPELINE:
        return lagfx_inner_bind_pipeline(p, payload, payload_len);
    case LAGFX_INNER_BIND_VERTEX_BUFFER:
        return lagfx_inner_bind_vertex_buffer(p, payload, payload_len);
    case LAGFX_INNER_BIND_FRAGMENT_RESOURCE:
        return lagfx_inner_bind_fragment_resource(p, payload, payload_len);
    case LAGFX_INNER_SET_RENDER_TARGET:
        return lagfx_inner_set_render_target(p, payload, payload_len);
    case LAGFX_INNER_DRAW:
        return lagfx_inner_draw(p, payload, payload_len);
    case LAGFX_INNER_SET_VIEWPORT:
        return lagfx_inner_set_viewport(p, payload, payload_len);
    case LAGFX_INNER_UNKNOWN:
    default:
        return lagfx_inner_unknown(p, inner_opcode, payload, payload_len);
    }
}

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
        lagfx_cmdbuf_commit_empty_vk_submit(p, LAGFX_OP_EXEC_INDIRECT2,
                                            hdr->stamp);
        return LAGFX_HANDLER_OK;
    }

    uint32_t task_id = lagfx_le32(hdr->payload + 0);
    uint32_t count   = lagfx_le32(hdr->payload + 4);

    /* count=0: explicit empty list. Complete cleanly + drive the Vulkan
     * queue round-trip (same rationale as the 0x22 count=0 path). */
    if (count == 0) {
        LAGFX_LOG("CmdExecIndirect2: taskID=%u count=0 "
                  "(empty-list completion) stamp=0x%08x",
                  task_id, hdr->stamp);
        lagfx_cmdbuf_commit_empty_vk_submit(p, LAGFX_OP_EXEC_INDIRECT2,
                                            hdr->stamp);
        return LAGFX_HANDLER_OK;
    }

    /* Non-empty: fail-open on unknown task. */
    lagfx_task_entry_t *task = lagfx_protocol_find_task(p, task_id);
    if (!task) {
        LAGFX_WARN("CmdExecIndirect2: taskID=%u not found "
                   "(continuing fail-open)", task_id);
    }

    /* Walk the inner stream. PARTIAL layout guess — each inner entry is
     * assumed to carry an 8-byte header {u32 inner_opcode, u32
     * inner_length} followed by inner_length - 8 payload bytes. The
     * inner_length field is inclusive of the 8-byte header. A single
     * malformed entry stops the walk; the outer stamp still signals
     * unconditionally through the dispatcher. */
    size_t cursor = 8u;  /* skip outer {taskID, count} */
    uint32_t processed = 0;
    while (processed < count) {
        if (cursor + 8u > hdr->payload_size) {
            LAGFX_WARN("CmdExecIndirect2: inner entry #%u header "
                       "overflow (cursor=%zu payload_size=%u)",
                       processed, cursor,
                       (unsigned)hdr->payload_size);
            return LAGFX_HANDLER_ERR_SIZE;
        }
        uint32_t inner_opcode = lagfx_le32(hdr->payload + cursor);
        uint32_t inner_length = lagfx_le32(hdr->payload + cursor + 4);
        if (inner_length < 8u ||
            (size_t)inner_length > (size_t)hdr->payload_size - cursor) {
            LAGFX_WARN("CmdExecIndirect2: inner entry #%u bad length "
                       "(inner_length=%u remaining=%zu)",
                       processed, inner_length,
                       (size_t)hdr->payload_size - cursor);
            return LAGFX_HANDLER_ERR_SIZE;
        }
        const uint8_t *inner_payload =
            hdr->payload + cursor + 8u;
        size_t inner_payload_len = (size_t)inner_length - 8u;

        (void)lagfx_process_inner(p, inner_opcode,
                                  inner_payload, inner_payload_len);

        cursor    += inner_length;
        processed += 1u;
    }

    LAGFX_LOG("CmdExecIndirect2: taskID=%u count=%u processed=%u "
              "stamp=0x%08x (Phase 3.A scaffold — inner-opcode dispatch)",
              task_id, count, processed, hdr->stamp);
    return LAGFX_HANDLER_OK;
}

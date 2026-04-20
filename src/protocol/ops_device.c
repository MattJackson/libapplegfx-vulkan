/*
 * libapplegfx-vulkan — device-domain opcode handlers
 * src/protocol/ops_device.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Phase 1.A.2 real handlers for the device-lifecycle opcodes:
 *
 *   CmdGetDeviceInfo    (0x0a) P0 — implemented (scaffold: recognized
 *     key table, no DMA writeback yet; see §2 below).
 *   CmdDefineTask2      (0x00) P0 — implemented (creates shell task,
 *     records mapping).
 *   CmdDeleteTask       (0x01) P0 — implemented.
 *   CmdMapMemory2       (0x02) P1 — stubbed (see TODO).
 *   CmdUnmapMemory      (0x03) P1 — stubbed (see TODO).
 *
 * Evidence anchors:
 *   - phase-1a2-decoder-plan.md §4.1 (P0 opcode arg layouts).
 *   - re-followup-spec-gaps.md §2 (GetDeviceInfo request triple;
 *     response format PARTIAL — 55% confidence).
 *
 * The dispatcher (protocol.c) writes the completion stamp + raises
 * the IRQ after the handler returns, unconditionally. Handlers here
 * only do opcode-specific work.
 */

#include "opcodes.h"
#include "protocol.h"
#include "state.h"
#include "../device.h"
#include "../common/log.h"

#include <string.h>

/* ===========================================================================
 * CmdGetDeviceInfo (0x0a) — P0
 *
 * Request layout (re-followup-spec-gaps.md §2.2, HIGH confidence 75%):
 *   payload[0..3]  u32 keyIndex    — which property the guest wants
 *   payload[4..7]  u32 outOffset   — response slot offset (partial)
 *   payload[8..11] u32 flags       — reserved
 *
 * Response layout (PARTIAL, 40% confidence per re-followup §2.3):
 *   The handler is expected to write the response value back into a
 *   guest-visible buffer. The most plausible interpretation is in-place
 *   writeback into the command's payload itself (which the guest kext
 *   then reads after the stamp completes). We do NOT have a
 *   shell.write_memory callback available at this layer, so we cannot
 *   perform an actual DMA writeback — we log the requested key + the
 *   hardcoded response value. When runtime capture closes the response
 *   gap (re-followup §2.5), this handler should be extended with a
 *   shell-side writeback hook.
 *
 * Until then: accept the query, map the key to a sane default, return
 * OK. Guest expectation for metal-no-op is that the stamp completes;
 * content of the response is best-effort.
 *
 * Key table (inferred from kext setupVersion() + _guestDeviceInfoMaxKey):
 *   0x0  protocol version
 *   0x1  maxTasks
 *   0x2  maxFIFOCount
 *   0x3  deviceFeatureLevel
 *   0x4  supports flags pack
 * =========================================================================== */

/* Little-endian u32 reader (host may be any endian; ring is LE). */
static inline uint32_t lagfx_le32(const uint8_t *b) {
    return (uint32_t)b[0]
         | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16)
         | ((uint32_t)b[3] << 24);
}

static inline uint64_t lagfx_le64(const uint8_t *b) {
    return (uint64_t)b[0]
         | ((uint64_t)b[1] << 8)
         | ((uint64_t)b[2] << 16)
         | ((uint64_t)b[3] << 24)
         | ((uint64_t)b[4] << 32)
         | ((uint64_t)b[5] << 40)
         | ((uint64_t)b[6] << 48)
         | ((uint64_t)b[7] << 56);
}

static uint32_t lagfx_device_info_for_key(uint32_t key) {
    switch (key) {
        case 0x0: return 1u;     /* protocol version 1 */
        case 0x1: return LAGFX_MAX_TASKS;      /* maxTasks */
        case 0x2: return LAGFX_MAX_CHILDFIFOS; /* maxFIFOCount */
        case 0x3: return 0u;     /* deviceFeatureLevel baseline */
        case 0x4: return 0u;     /* no optional features */
        default:  return 0u;     /* unknown key → 0 */
    }
}

lagfx_handler_status_t lagfx_op_get_device_info(lagfx_protocol_t *p,
                                                const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    /* Descriptor-table min_payload=12 already gates this, but defend
     * anyway — if payload isn't present in the buffer (caller passed
     * only the header), we can't read args. */
    if (!hdr->payload || hdr->payload_size < 12) {
        LAGFX_WARN("CmdGetDeviceInfo: payload missing or truncated "
                   "(size=%u, have=%p)", (unsigned)hdr->payload_size,
                   (const void *)hdr->payload);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint32_t key        = lagfx_le32(hdr->payload + 0);
    uint32_t out_offset = lagfx_le32(hdr->payload + 4);
    uint32_t flags      = lagfx_le32(hdr->payload + 8);
    uint32_t value      = lagfx_device_info_for_key(key);

    LAGFX_LOG("CmdGetDeviceInfo: stamp=0x%08x key=0x%x out_off=0x%x "
              "flags=0x%x -> value=0x%x (response writeback deferred)",
              hdr->stamp, key, out_offset, flags, value);

    /* TODO(Phase-1.A.3): once the response writeback target is closed
     * by runtime capture (re-followup §2.5), emit the 64-byte
     * zero-filled response or single-u32 writeback via a new
     * shell.write_memory callback. For now the stamp alone signals
     * completion, which is sufficient for the metal-no-op control-plane
     * round-trip; kext paths that read the response value will see the
     * original payload bytes unchanged. */
    (void)value;
    (void)out_offset;
    (void)flags;
    return LAGFX_HANDLER_OK;
}

/* ===========================================================================
 * CmdDefineTask2 (0x00) — P0
 *
 * Request layout (phase-1a2-decoder-plan.md §4.1, HIGH confidence):
 *   payload[0..3]   u32 taskID
 *   payload[4..11]  u64 rootVA
 *   payload[12..19] u64 length
 *   payload[20..23] u32 reserved
 *
 * Invokes the shell.create_task callback with vm_size=length, records
 * {taskID, shell_task, base_va, length} in p->tasks, marks live.
 * On slot exhaustion returns LAGFX_HANDLER_ERR_STATE but still signals
 * completion (dispatcher). Duplicate taskID reuses the existing slot
 * and logs a warning.
 * =========================================================================== */

lagfx_handler_status_t lagfx_op_define_task2(lagfx_protocol_t *p,
                                             const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    if (!hdr->payload || hdr->payload_size < 24) {
        LAGFX_WARN("CmdDefineTask2: payload missing or too small "
                   "(size=%u)", (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint32_t task_id  = lagfx_le32(hdr->payload + 0);
    uint64_t root_va  = lagfx_le64(hdr->payload + 4);
    uint64_t length   = lagfx_le64(hdr->payload + 12);
    uint32_t reserved = lagfx_le32(hdr->payload + 20);
    (void)reserved;

    /* Duplicate: re-use slot. */
    lagfx_task_entry_t *entry = lagfx_protocol_find_task(p, task_id);
    if (entry) {
        LAGFX_WARN("CmdDefineTask2: duplicate taskID=%u (re-using slot)",
                   task_id);
    } else {
        entry = lagfx_protocol_alloc_task_slot(p);
        if (!entry) {
            LAGFX_WARN("CmdDefineTask2: task table full (max=%u)",
                       LAGFX_MAX_TASKS);
            return LAGFX_HANDLER_ERR_STATE;
        }
    }

    /* Invoke shell create_task if available AND length is non-zero;
     * otherwise record the taskID with a NULL handle (per decoder-plan
     * §9 R8: shell-side create_task may return NULL; decoder still
     * tracks the taskID). Length=0 is valid scaffold input during
     * bring-up (tests exercise min_payload zero-buffers); we record
     * the mapping without asking the shell to reserve VA. */
    lagfx_task_t *shell_task = NULL;
    void         *base_ptr   = NULL;
    if (length > 0 && p->dev && p->dev->desc.shell.create_task) {
        shell_task = p->dev->desc.shell.create_task(
            p->dev->desc.shell.opaque,
            length,
            &base_ptr);
        if (!shell_task) {
            LAGFX_WARN("CmdDefineTask2: shell.create_task returned NULL "
                       "for taskID=%u (proceeding defensively per R8)",
                       task_id);
        }
    } else if (length == 0) {
        LAGFX_LOG("CmdDefineTask2: taskID=%u length=0 — recording slot "
                  "without shell.create_task", task_id);
    } else {
        LAGFX_WARN("CmdDefineTask2: no shell.create_task callback; "
                   "taskID=%u recorded without backing", task_id);
    }

    entry->id         = task_id;
    entry->shell_task = shell_task;
    /* base_va semantics: prefer guest-reported rootVA for correlation;
     * store shell's returned base_ptr lower-bits as fallback when
     * rootVA == 0. */
    entry->base_va    = root_va ? root_va : (uint64_t)(uintptr_t)base_ptr;
    entry->length     = length;
    entry->live       = true;

    LAGFX_LOG("CmdDefineTask2: taskID=%u rootVA=0x%llx length=%llu "
              "shell_task=%p stamp=0x%08x",
              task_id,
              (unsigned long long)root_va,
              (unsigned long long)length,
              (void *)shell_task,
              hdr->stamp);
    return LAGFX_HANDLER_OK;
}

/* ===========================================================================
 * CmdDeleteTask (0x01) — P0
 *
 * Request layout (plan §4.1 HIGH): payload[0..3] u32 taskID.
 * Looks up p->tasks; calls shell.destroy_task if shell_task!=NULL;
 * marks entry !live. Error if taskID not found.
 * =========================================================================== */

lagfx_handler_status_t lagfx_op_delete_task(lagfx_protocol_t *p,
                                            const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    if (!hdr->payload || hdr->payload_size < 4) {
        LAGFX_WARN("CmdDeleteTask: payload missing or too small "
                   "(size=%u)", (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint32_t task_id = lagfx_le32(hdr->payload + 0);

    lagfx_task_entry_t *entry = lagfx_protocol_find_task(p, task_id);
    if (!entry) {
        LAGFX_WARN("CmdDeleteTask: taskID=%u not found", task_id);
        return LAGFX_HANDLER_ERR_STATE;
    }

    if (entry->shell_task && p->dev && p->dev->desc.shell.destroy_task) {
        p->dev->desc.shell.destroy_task(p->dev->desc.shell.opaque,
                                        entry->shell_task);
    }

    LAGFX_LOG("CmdDeleteTask: taskID=%u stamp=0x%08x", task_id, hdr->stamp);

    memset(entry, 0, sizeof(*entry));
    entry->live = false;
    return LAGFX_HANDLER_OK;
}

/* ===========================================================================
 * CmdMapMemory2 (0x02) — P1 (deferred to Phase 1.A.3)
 *
 * Request layout (plan §4.2 + command-buffer-format.md §4): variable
 * length, tentatively:
 *   u32 taskID, u64 vm_offset, u32 read_only, u32 range_count,
 *   lagfx_physical_range_t ranges[range_count]
 *
 * Re-followup-spec-gaps.md does NOT cover this opcode — layout remains
 * MEDIUM confidence. Kext may emit it during child-FIFO ring backing;
 * metal-no-op probably does NOT require it to succeed (empty cmdbuf
 * path short-circuits through CmdSynchronizeResources).
 *
 * Leaving this stubbed so the dispatcher still completes the stamp
 * (fail-open) and we observe whether real guest traffic fires it.
 * =========================================================================== */

lagfx_handler_status_t lagfx_op_map_memory2(lagfx_protocol_t *p,
                                            const lagfx_cmd_header_t *hdr) {
    (void)p;
    LAGFX_LOG("CmdMapMemory2: TODO(P1) stamp=0x%08x payload_size=%u — "
              "layout unconfirmed; see re-followup-spec-gaps.md gap list",
              hdr ? hdr->stamp : 0u,
              hdr ? (unsigned)hdr->payload_size : 0u);
    return LAGFX_HANDLER_OK;
}

/* ===========================================================================
 * CmdUnmapMemory (0x03) — P1 (deferred to Phase 1.A.3)
 *
 * Request layout per plan §4.2: {u32 taskID, u64 vm_offset, u64 length}
 * (20 bytes). Same confidence caveat as CmdMapMemory2.
 * =========================================================================== */

lagfx_handler_status_t lagfx_op_unmap_memory(lagfx_protocol_t *p,
                                             const lagfx_cmd_header_t *hdr) {
    (void)p;
    LAGFX_LOG("CmdUnmapMemory: TODO(P1) stamp=0x%08x payload_size=%u — "
              "layout unconfirmed; see re-followup-spec-gaps.md gap list",
              hdr ? hdr->stamp : 0u,
              hdr ? (unsigned)hdr->payload_size : 0u);
    return LAGFX_HANDLER_OK;
}

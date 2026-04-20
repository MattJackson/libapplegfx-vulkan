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
 * CmdMapMemory2 (0x02) — P1 (Phase 1.A.2)
 *
 * Request layout (command-buffer-format.md §4 "Variable-Length Arrays",
 * PARTIAL confidence per re-followup-spec-gaps.md — re-followup did not
 * decode this opcode, so the shape inherits from the pre-v1.2 spec and
 * the dylib handler signature implied by the shell.map_memory callback
 * at libapplegfx-vulkan.h:110):
 *
 *     payload[0..3]   u32 taskID
 *     payload[4..11]  u64 virtualOffset
 *     payload[12..15] u32 readOnly  (bool, lsb meaningful)
 *     payload[16..19] u32 rangeCount
 *     payload[20..]   struct { u64 gpa; u64 length; } ranges[rangeCount]
 *
 * Semantics (scaffolded against shell callback):
 *   - Look up the task entry by taskID (fail-open on miss — log and
 *     continue with a NULL shell_task; Apple's memory-model.md §2 shows
 *     the shell callback can reject per-range and we surface that).
 *   - Invoke shell.map_memory ONCE with the full ranges array — the
 *     callback's own contract (memory-model.md §2, "Multi-Range Batch
 *     Mapping") is to loop per-range and advance virtual_offset
 *     internally. We do NOT advance virtualOffset ourselves per range;
 *     the host callback owns that advance (see memory-model.md §2
 *     line "Advance virtual_offset by the range length").
 *   - Completion: unconditional via the dispatcher. Mid-list failure
 *     returns LAGFX_HANDLER_ERR_STATE but the stamp still signals.
 *
 * NOTE: re-followup-spec-gaps.md does not re-verify the 20-byte prefix;
 * runtime capture (§2.5) will be needed to confirm field ordering.
 * Flagged PARTIAL in opcode descriptor comments.
 * =========================================================================== */

lagfx_handler_status_t lagfx_op_map_memory2(lagfx_protocol_t *p,
                                            const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    if (!hdr->payload || hdr->payload_size < 20) {
        LAGFX_WARN("CmdMapMemory2: payload missing or too small "
                   "(size=%u, need >= 20)", (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint32_t task_id        = lagfx_le32(hdr->payload + 0);
    uint64_t virtual_offset = lagfx_le64(hdr->payload + 4);
    uint32_t read_only      = lagfx_le32(hdr->payload + 12);
    uint32_t range_count    = lagfx_le32(hdr->payload + 16);

    /* Overflow-safe size check: each range is 16 bytes (u64 gpa + u64 len).
     * Required payload: 20 + 16*range_count. Mirrors dylib-style
     * multiplicative-overflow guards (cf. CmdSynchronizeResources §4.3). */
    if (range_count > ((uint32_t)hdr->payload_size - 20u) / 16u) {
        LAGFX_WARN("CmdMapMemory2: range_count=%u exceeds payload "
                   "(size=%u, need >= %u)",
                   range_count, (unsigned)hdr->payload_size,
                   20u + 16u * range_count);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    /* Look up task. Unknown taskID: log but continue — the map callback
     * may still accept a NULL task handle (memory-model.md §1 shows
     * apple_gfx_create_task can return a stub handle in bring-up; the
     * shell may map against a default/root task). Fail-open. */
    lagfx_task_entry_t *task = lagfx_protocol_find_task(p, task_id);
    if (!task) {
        LAGFX_WARN("CmdMapMemory2: taskID=%u not found "
                   "(continuing fail-open)", task_id);
    }

    /* Empty ranges: degenerate case — still a valid completion. */
    if (range_count == 0) {
        LAGFX_LOG("CmdMapMemory2: taskID=%u vm_off=0x%llx ro=%u "
                  "range_count=0 (no-op map) stamp=0x%08x",
                  task_id, (unsigned long long)virtual_offset,
                  read_only & 1u, hdr->stamp);
        return LAGFX_HANDLER_OK;
    }

    /* Stack-assemble the ranges array for the callback. The on-wire
     * field order is (gpa, length) per 16-byte slot — matches
     * lagfx_physical_range_t exactly (libapplegfx-vulkan.h:88). */
    enum { LAGFX_MAP_MAX_RANGES = 64 };
    if (range_count > LAGFX_MAP_MAX_RANGES) {
        LAGFX_WARN("CmdMapMemory2: range_count=%u exceeds batch cap %u "
                   "(truncating would lose mappings — rejecting)",
                   range_count, LAGFX_MAP_MAX_RANGES);
        return LAGFX_HANDLER_ERR_SIZE;
    }
    lagfx_physical_range_t ranges[LAGFX_MAP_MAX_RANGES];
    for (uint32_t i = 0; i < range_count; ++i) {
        const uint8_t *r = hdr->payload + 20u + 16u * i;
        ranges[i].guest_physical_address = lagfx_le64(r + 0);
        ranges[i].length                 = lagfx_le64(r + 8);
    }

    lagfx_handler_status_t status = LAGFX_HANDLER_OK;
    if (p->dev && p->dev->desc.shell.map_memory) {
        lagfx_task_t *shell_task = task ? task->shell_task : NULL;
        bool ok = p->dev->desc.shell.map_memory(
            p->dev->desc.shell.opaque,
            shell_task,
            virtual_offset,
            ranges,
            (size_t)range_count,
            (read_only & 1u) != 0u);
        if (!ok) {
            LAGFX_WARN("CmdMapMemory2: shell.map_memory returned false "
                       "for taskID=%u vm_off=0x%llx range_count=%u "
                       "(completing stamp anyway — fail-open)",
                       task_id, (unsigned long long)virtual_offset,
                       range_count);
            status = LAGFX_HANDLER_ERR_STATE;
        }
    } else {
        LAGFX_WARN("CmdMapMemory2: no shell.map_memory callback; "
                   "taskID=%u treated as success (scaffold)", task_id);
    }

    LAGFX_LOG("CmdMapMemory2: taskID=%u vm_off=0x%llx ro=%u "
              "range_count=%u stamp=0x%08x status=%d",
              task_id, (unsigned long long)virtual_offset,
              read_only & 1u, range_count, hdr->stamp, (int)status);
    return status;
}

/* ===========================================================================
 * CmdUnmapMemory (0x03) — P1 (Phase 1.A.2)
 *
 * Request layout (plan §4.2, PARTIAL confidence — re-followup did not
 * decode this opcode; shape inherits from pre-v1.2 spec and matches the
 * shell.unmap_memory signature at libapplegfx-vulkan.h:116):
 *
 *     payload[0..3]    u32 taskID
 *     payload[4..11]   u64 virtualOffset
 *     payload[12..19]  u64 length
 *
 * Total payload: 20 bytes (exact, per opcode descriptor).
 *
 * Semantics: look up task, call shell.unmap_memory(task, vm_off, length).
 * Unknown taskID is fail-open (logged, continues with NULL shell_task).
 * =========================================================================== */

lagfx_handler_status_t lagfx_op_unmap_memory(lagfx_protocol_t *p,
                                             const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    if (!hdr->payload || hdr->payload_size < 20) {
        LAGFX_WARN("CmdUnmapMemory: payload missing or too small "
                   "(size=%u, need 20)", (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint32_t task_id        = lagfx_le32(hdr->payload + 0);
    uint64_t virtual_offset = lagfx_le64(hdr->payload + 4);
    uint64_t length         = lagfx_le64(hdr->payload + 12);

    lagfx_task_entry_t *task = lagfx_protocol_find_task(p, task_id);
    if (!task) {
        LAGFX_WARN("CmdUnmapMemory: taskID=%u not found "
                   "(continuing fail-open)", task_id);
    }

    lagfx_handler_status_t status = LAGFX_HANDLER_OK;
    if (p->dev && p->dev->desc.shell.unmap_memory) {
        lagfx_task_t *shell_task = task ? task->shell_task : NULL;
        bool ok = p->dev->desc.shell.unmap_memory(
            p->dev->desc.shell.opaque,
            shell_task,
            virtual_offset,
            length);
        if (!ok) {
            LAGFX_WARN("CmdUnmapMemory: shell.unmap_memory returned false "
                       "for taskID=%u vm_off=0x%llx length=%llu "
                       "(completing stamp anyway — fail-open)",
                       task_id, (unsigned long long)virtual_offset,
                       (unsigned long long)length);
            status = LAGFX_HANDLER_ERR_STATE;
        }
    } else {
        LAGFX_WARN("CmdUnmapMemory: no shell.unmap_memory callback; "
                   "taskID=%u treated as success (scaffold)", task_id);
    }

    LAGFX_LOG("CmdUnmapMemory: taskID=%u vm_off=0x%llx length=%llu "
              "stamp=0x%08x status=%d",
              task_id, (unsigned long long)virtual_offset,
              (unsigned long long)length, hdr->stamp, (int)status);
    return status;
}

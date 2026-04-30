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
#include "resource_registry.h"
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
              "flags=0x%x -> value=0x%x",
              hdr->stamp, key, out_offset, flags, value);

    /* DMA the single-u32 value to the caller's out_offset GPA. The
     * exact response shape at this opcode (0x0a) was inferred from
     * the dylib side. The KEXT consumer (AppleParavirtAccelerator)
     * uses opcode 0x2a with a different response shape — see
     * paravirt-re/re-followup-spec-gaps.md §12. Handled separately
     * in lagfx_op_accelerator_device_info (TODO). */
    if (p->dev && p->dev->desc.shell.write_memory && out_offset != 0u) {
        if (!p->dev->desc.shell.write_memory(p->dev->desc.shell.opaque,
                                             (uint64_t)out_offset,
                                             sizeof(value), &value)) {
            LAGFX_WARN("CmdGetDeviceInfo: DMA writeback failed "
                       "(gpa=0x%x value=0x%x)", out_offset, value);
            return LAGFX_HANDLER_ERR_INTERNAL;
        }
    }

    (void)flags;
    return LAGFX_HANDLER_OK;
}

/* ===========================================================================
 * CmdGetDeviceInfo2 (0x3a) — M2+ extended opcode — M3 GATE
 *
 * Emitted by `AppleParavirtAccelerator::setupDeviceInfo()` at kext
 * vmaddr 0x14557806. Must complete before `registerService()` fires
 * and thus before `MTLCreateSystemDefaultDevice()` returns non-null.
 * See paravirt-re/re-followup-spec-gaps.md §13.2.4.
 *
 * Request payload (12 bytes):
 *   +0  u32 kind          observed 0x2a
 *   +4  u32 resp_qwords   response buffer size in 8-byte units (observed 0x200)
 *   +8  u32 resp_pfn      GPA of response buffer >> 12
 *
 * Response: fill resp_pfn << 12 with key-tagged tuples
 *   struct { u32 key; u32 value; } pairs[<=resp_qwords]
 *
 * Keys 0x01..0x29 — parser at kext vmaddr 0x1455d41d. The struct is
 * bulk-copied to userspace as the Metal dylib's _deviceInfo (216B).
 * Field names from ObjC type encoding (authoritative source):
 *   0x01=MSAASamples, 0x02=D24S8Supported, 0x03=MaxThreadsPerTG_W,
 *   0x04=MaxThreadsPerTG_H, 0x05=MaxThreadsPerTG_D, ... see pairs[]
 *   below for the full mapping. Key 0x01 MUST be >= 1 or
 *   supportsRasterSampleCount:1 returns NO (M5-10% crash gate).
 * =========================================================================== */
lagfx_handler_status_t
lagfx_op_get_device_info_2(lagfx_protocol_t *p,
                           const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    if (!hdr->payload || hdr->payload_size < 12u) {
        LAGFX_WARN("CmdGetDeviceInfo2: payload too small (%u)",
                   (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint32_t kind        = lagfx_le32(hdr->payload + 0);
    uint32_t resp_qwords = lagfx_le32(hdr->payload + 4);
    uint32_t resp_pfn    = lagfx_le32(hdr->payload + 8);
    uint64_t resp_gpa    = (uint64_t)resp_pfn << 12;

    LAGFX_LOG("CmdGetDeviceInfo2: kind=0x%x resp_qwords=0x%x resp_pfn=0x%x "
              "-> gpa=0x%llx",
              kind, resp_qwords, resp_pfn, (unsigned long long)resp_gpa);

    if (!p->dev || !p->dev->desc.shell.write_memory || resp_pfn == 0u) {
        LAGFX_WARN("CmdGetDeviceInfo2: no write_memory or resp_pfn=0; "
                   "stamping with empty response");
        (void)kind;
        (void)resp_qwords;
        return LAGFX_HANDLER_OK;
    }

    /*
     * Full TLV superset (M5, 2026-04-27). The parser at 0x1455d41d
     * dispatches tag in [0, 0x29] via a jump table; every handler is
     * a pure u32 store to [accel+0xe68 + (tag-1)*4] plus a presence
     * flag at [accel+0xe68 + 0xa4 + (tag-1)]. Missing tags leave
     * their destination field as zero; invalid values pass unchanged.
     *
     * The 216-byte APVDeviceInfoStruct is bulk-copied to userspace
     * via AppleParavirtShared::getDeviceInfo (memcpy 0xd8 bytes) and
     * becomes the Metal dylib's _deviceInfo struct. Field names below
     * are from the ObjC type encoding of _deviceInfo (authoritative).
     *
     * Tag 0x12 has post-parse defaulting: the parser forces 0x20002
     * if the tag is missing, 0x20007 if the supplied value < 0x20008.
     * Emit 0x20008 so populateAccelConfig sees a "modern" version.
     *
     * M5-10% gate: tag 0x01 (MSAASamples) MUST be >= 1 or
     * supportsRasterSampleCount:1 returns NO and CoreAnimation aborts.
     * The method checks (MSAASamples.u >= sampleCount) after gating
     * against bitmask 0x10116 (valid rates: 1,2,4,8,16). Value 4
     * means supports {1x, 2x, 4x}.
     *
     * Host cost: 41 u32-pair writes = 328 bytes of DMA.
     */
    struct { uint32_t key; uint32_t value; } pairs[] = {
        { 0x01, 4          }, /* MSAASamples: max MSAA rate 4x */
        { 0x02, 1          }, /* D24S8Supported: depth24_stencil8 */
        { 0x03, 1024       }, /* MaxThreadsPerThreadgroupW */
        { 0x04, 1024       }, /* MaxThreadsPerThreadgroupH */
        { 0x05, 1024       }, /* MaxThreadsPerThreadgroupD */
        { 0x06, 32768      }, /* MaxThreadgroupMemoryLength */
        { 0x07, 1          }, /* IsFramebufferReadSupported */
        { 0x08, 1          }, /* IsRGB10A2GammaSupported */
        { 0x09, 0          }, /* SupportsNativeHardwareFP16 */
        { 0x0a, 0x00020008 }, /* DeserializerVersion */
        { 0x0b, 0x7f       }, /* PrimitiveTypeSupport: all standard */
        { 0x0c, 0          }, /* SupportsMultiplaneTextures */
        { 0x0d, 256        }, /* LinearTextureAlignment */
        { 0x0e, 1          }, /* HeapBuffers */
        { 0x0f, 256        }, /* HeapBufferAlignment */
        { 0x10, 1          }, /* HeapTextures */
        { 0x11, 1          }, /* BufferFromIOSurface */
        { 0x12, 0x00020008 }, /* MaxMetalShaderVersion — modern path */
        { 0x13, 0          }, /* SupportsSharedTextures */
        { 0x14, 1          }, /* MaxVertexAmplificationCount */
        { 0x15, 0          }, /* SupportsProgrammableSamplePositions */
        { 0x16, 0          }, /* RasterizationRateLayerCount */
        { 0x17, 0          }, /* TileShaders */
        { 0x18, 0          }, /* ImageBlocks */
        { 0x19, 0          }, /* RasterOrderGroups */
        { 0x1a, 0          }, /* MemoryOrderAtomics */
        { 0x1b, 0          }, /* LargeMRT */
        { 0x1c, 0          }, /* SupportFlags2023 */
        { 0x1d, 1024       }, /* MaxTotalComputeThreadsPerThreadgroup */
        { 0x1e, 32768      }, /* MaxComputeLocalMemorySizes */
        { 0x1f, 32768      }, /* MaxComputeThreadgroupMemory */
        { 0x20, 256        }, /* MaxComputeThreadgroupMemoryAlignmentBytes */
        { 0x21, 0          }, /* SupportFlags2024 */
        { 0x22, 1          }, /* GpuCoreCount */
        { 0x23, 2048       }, /* MaxTextureLayers */
        { 0x24, 0          }, /* MaxPredicatedNestingDepth */
        { 0x25, 0x10005    }, /* HostGPUFamily: Apple5 (Intel-like) */
        { 0x26, 1          }, /* ArgumentBuffersTier */
        { 0x27, 1024       }, /* ArgumentBuffersMaxSamplerCount */
        { 0x28, 256        }, /* MinimumLinearTextureAlignment */
        { 0x29, 0          }, /* SupportedTextureWriteRoundingModes */
    };
    const size_t n_pairs = sizeof(pairs) / sizeof(pairs[0]);

    /* Clamp to guest-provided capacity (should always be ample — 41
     * pairs = 328 bytes vs observed 0x200 qwords = 4 KiB). */
    size_t emit_pairs =
        (n_pairs * 8u <= resp_qwords * 8u) ? n_pairs : resp_qwords;

    if (!p->dev->desc.shell.write_memory(p->dev->desc.shell.opaque,
                                          resp_gpa,
                                          emit_pairs * sizeof(pairs[0]),
                                          pairs)) {
        LAGFX_WARN("CmdGetDeviceInfo2: DMA writeback failed to gpa=0x%llx",
                   (unsigned long long)resp_gpa);
        return LAGFX_HANDLER_ERR_INTERNAL;
    }

    LAGFX_LOG("CmdGetDeviceInfo2: wrote %zu pairs to resp page", emit_pairs);

    /*
     * The kext's setupDeviceInfo reloads actual_count from the on-ring
     * header's length slot (+4) via `mov esi, [r13+4]` AFTER the stamp
     * completion fires. We must DMA that value back BEFORE the stamp
     * path runs — complete_stamp() is called by the dispatcher
     * immediately after this handler returns, so we cannot rely on the
     * drain to do the writeback. Inline it here.
     */
    uint32_t actual_count = (uint32_t)emit_pairs;
    p->device_info_actual_count = actual_count;
    if (p->current_cmd_header_gpa != 0) {
        uint64_t len_slot_gpa = p->current_cmd_header_gpa + 4u;
        if (!p->dev->desc.shell.write_memory(p->dev->desc.shell.opaque,
                                              len_slot_gpa,
                                              sizeof(actual_count),
                                              &actual_count)) {
            LAGFX_WARN("CmdGetDeviceInfo2: failed to write actual_count=%u "
                       "to ring header +4 (gpa=0x%llx)",
                       actual_count, (unsigned long long)len_slot_gpa);
        } else {
            LAGFX_LOG("CmdGetDeviceInfo2: wrote actual_count=%u to ring +4 "
                      "(gpa=0x%llx)",
                      actual_count, (unsigned long long)len_slot_gpa);
        }
    } else {
        LAGFX_WARN("CmdGetDeviceInfo2: no current_cmd_header_gpa "
                   "— actual_count writeback skipped");
    }

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

    lagfx_resource_clear_task(&p->resources, task_id);

    memset(entry, 0, sizeof(*entry));
    entry->live = false;
    return LAGFX_HANDLER_OK;
}

/* ===========================================================================
 * CmdDefineHostTask (0x38) — P0
 *
 * Wire format CONFIRMED via live capture + annotated disasm of
 * AppleParavirtTask::defineHostTask (kext+0x14558dfa):
 *
 *   payload[0..3]   u32 slot_index      (= taskID << 1; bit 0 is a
 *                      stamp-completion flag, always 0 on emit)
 *   payload[4..11]  u64 taskHandleHint   (observed 0x0000000400000000)
 *   payload[12..15] u32 root_page_pfn    (PFN of shared-header page)
 *
 * The shared-header page at (root_page_pfn << 12) contains:
 *   +0x00  u32 L1_pfn  (PFN of radix-tree root interior node)
 *   +0x04  u32 levels  (always 3 for typical tasks)
 *   +0x08  u64 reserved0
 *   +0x10  u32 reserved1
 *   +0x14  u64 taskHandleHint
 * See paravirt-re/library/state-machines/per-task-page-table.md.
 *
 * Semantics: register/refresh a task's host-side radix page-table root.
 * The kext re-emits this opcode periodically (~320 fires per boot
 * observed); we treat each as authoritative — overwrite any prior
 * root_page_pfn for the same task_id. Allocate a task slot if the
 * task_id wasn't already registered (CmdDefineTask2 may not have
 * fired for vchan/exec-channel tasks).
 * =========================================================================== */

lagfx_handler_status_t lagfx_op_set_resource_heap(lagfx_protocol_t *p,
                                                   const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    if (!hdr->payload || hdr->payload_size < 12u) {
        LAGFX_WARN("CmdSetResourceHeap: payload missing or too small "
                   "(size=%u, need >= 12)", (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint32_t task_id   = lagfx_le32(hdr->payload + 0);
    uint32_t heap_pfn  = lagfx_le32(hdr->payload + 4);
    uint32_t heap_size = lagfx_le32(hdr->payload + 8);

    lagfx_task_entry_t *entry = lagfx_protocol_find_task(p, task_id);
    if (!entry) {
        LAGFX_WARN("CmdSetResourceHeap: taskID=%u not found", task_id);
        return LAGFX_HANDLER_OK;
    }

    entry->heap_pfn  = heap_pfn;
    entry->heap_size = heap_size;

    LAGFX_TRACE("CmdSetResourceHeap: taskID=%u heap_pfn=0x%x "
              "heap_size=0x%x stamp=0x%08x",
              task_id, heap_pfn, heap_size, hdr->stamp);
    return LAGFX_HANDLER_OK;
}

lagfx_handler_status_t lagfx_op_define_host_task(lagfx_protocol_t *p,
                                                 const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    if (!hdr->payload || hdr->payload_size < 16) {
        LAGFX_WARN("CmdDefineHostTask: payload missing or too small "
                   "(size=%u, need >= 16)", (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    uint32_t slot_index      = lagfx_le32(hdr->payload + 0);
    uint32_t reserved       = lagfx_le32(hdr->payload + 4);
    uint32_t flags          = lagfx_le32(hdr->payload + 8);
    uint32_t root_page_pfn  = lagfx_le32(hdr->payload + 12);
    (void)reserved;
    (void)flags;

    uint32_t task_id = slot_index >> 1;

    lagfx_task_entry_t *entry = lagfx_protocol_find_task(p, task_id);
    if (!entry) {
        entry = lagfx_protocol_alloc_task_slot(p);
        if (!entry) {
            LAGFX_WARN("CmdDefineHostTask: task table full (max=%u)",
                       LAGFX_MAX_TASKS);
            return LAGFX_HANDLER_ERR_STATE;
        }
        entry->id = task_id;
        entry->live = true;
    }
    /* Overwriting root_page_pfn is correct on slot reuse: the old radix
     * pages have been freed by the kext, so the next translate must
     * walk the new tree. No additional state to clear. */
    entry->root_page_pfn = root_page_pfn;

    LAGFX_LOG("CmdDefineHostTask: taskID=%u root_page_pfn=0x%x "
              "handleHint=0x%llx stamp=0x%08x",
              task_id, root_page_pfn,
              (unsigned long long)((uint64_t)lagfx_le32(hdr->payload + 4)
                                   | ((uint64_t)lagfx_le32(hdr->payload + 8) << 32)),
              hdr->stamp);

    if (p->dev && p->dev->desc.shell.read_memory) {
        uint64_t hdr_gpa = ((uint64_t)root_page_pfn << 12);
        uint8_t hdr[32] = {0};
        if (p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                           hdr_gpa, sizeof(hdr), hdr)) {
            char hex[32 * 3 + 1];
            size_t pos = 0;
            for (size_t i = 0; i < 28u; ++i) {
                int w = snprintf(hex + pos, sizeof(hex) - pos, "%02x ",
                                 (unsigned)hdr[i]);
                if (w > 0) pos += (size_t)w;
            }
            uint32_t hdr_l1 = (uint32_t)hdr[0]
                              | ((uint32_t)hdr[1] << 8)
                              | ((uint32_t)hdr[2] << 16)
                              | ((uint32_t)hdr[3] << 24);
            uint32_t hdr_levels = (uint32_t)hdr[4]
                                  | ((uint32_t)hdr[5] << 8)
                                  | ((uint32_t)hdr[6] << 16)
                                  | ((uint32_t)hdr[7] << 24);
            LAGFX_LOG("  root header @0x%llx: %s l1=0x%x levels=%u",
                      (unsigned long long)hdr_gpa, hex,
                      hdr_l1, hdr_levels);
        }
    }

    return LAGFX_HANDLER_OK;
}

/* ===========================================================================
 * CmdMapMemoryImmediate (kext opcode 0x39 on the Immediate vchan) — P0
 *
 * Confirmed via paravirt-re/library/journey/opcodes-0x35-0x36-0x39.md
 * and the annotated AppleParavirtMemoryMap-commitIntoGPUPageTable.asm.
 * This opcode publishes a (taskID, vaBase, vaLength) declaration after
 * the kext commits memory into the GPU page-table. The kext writes
 * PTEs into the radix tree at task->root_page_pfn directly via guest
 * CPU stores (per AppleParavirtPageTable-StorageNode-setEntry); the
 * host walks those PTEs in lagfx_task_translate. So 0x39 itself does
 * NOT carry GPA data the host needs — translation is via the radix
 * tree, not this opcode.
 *
 * Wire format (20-byte trailer; optional preceding scatter blocks):
 *
 *   [scatter_block_0][scatter_block_1][scatter_block_2]
 *   <trailer at payload tail (last 20 bytes):
 *       +0x00  u32 task_id
 *       +0x04  u64 vaBase       (guest-kernel VA into IOMemoryMap)
 *       +0x0c  u64 vaLength
 *
 * Boot/runtime traces observed have zero-length scatter prefix because
 * the per-resource scatter table is empty / single-PA (per the
 * commitIntoGPUPageTable RE: emit() calls are no-ops in single-PA
 * mode). We still log any prefix bytes for future RE in case a
 * multi-PA case ever shows up on the wire.
 * =========================================================================== */

lagfx_handler_status_t lagfx_op_map_memory_immediate(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    if (!hdr->payload || hdr->payload_size < 20u) {
        LAGFX_WARN("CmdMapMemoryImmediate: payload missing or too small "
                   "(size=%u, need >= 20)", (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    /* Trailer is at the END of the payload (last 20 bytes). */
    size_t off = (size_t)hdr->payload_size - 20u;
    uint32_t task_id  = lagfx_le32(hdr->payload + off + 0);
    uint64_t va_base  = (uint64_t)lagfx_le32(hdr->payload + off + 4)
                        | ((uint64_t)lagfx_le32(hdr->payload + off + 8) << 32);
    uint64_t va_len   = (uint64_t)lagfx_le32(hdr->payload + off + 12)
                        | ((uint64_t)lagfx_le32(hdr->payload + off + 16) << 32);

    /* Log any scatter prefix so future RE has data when a multi-PA
     * sample finally lands on the wire. */
    if (off > 0) {
        size_t n = (off < 64u) ? off : 64u;
        char line[64 * 4 + 8];
        size_t lpos = 0;
        for (size_t i = 0; i < n; ++i) {
            int x = snprintf(line + lpos, sizeof(line) - lpos, "%02x ",
                             hdr->payload[i]);
            if (x <= 0 || (size_t)x >= sizeof(line) - lpos) break;
            lpos += (size_t)x;
        }
        LAGFX_TRACE("CmdMapMemoryImmediate: scatter prefix[%zu]: %s",
                  off, line);
    }

    LAGFX_TRACE("CmdMapMemoryImmediate: taskID=%u vaBase=0x%llx "
              "vaLength=0x%llx stamp=0x%08x",
              task_id, (unsigned long long)va_base,
              (unsigned long long)va_len, hdr->stamp);

    lagfx_task_entry_t *entry = lagfx_protocol_find_task(p, task_id);
    if (!entry) {
        LAGFX_WARN("CmdMapMemoryImmediate: taskID=%u not found "
                   "(interval not registered)", task_id);
        return LAGFX_HANDLER_OK;
    }

    {
        uint64_t new_end = va_base + va_len;
        lagfx_va_interval_t *target_iv = NULL;
        bool merged = false;
        for (uint32_t i = 0; i < entry->va_interval_count; ++i) {
            lagfx_va_interval_t *iv = &entry->va_intervals[i];
            uint64_t iv_end = iv->va_base + iv->length;
            if (va_base <= iv_end && iv->va_base <= new_end) {
                uint64_t mb = va_base < iv->va_base ? va_base : iv->va_base;
                uint64_t me = new_end  > iv_end    ? new_end  : iv_end;
                iv->va_base  = mb;
                iv->gpa_base = mb;
                iv->length   = me - mb;
                target_iv = iv;
                merged = true;
                break;
            }
        }
        if (!merged) {
            if (entry->va_interval_count < LAGFX_MAX_VA_INTERVALS) {
                target_iv =
                    &entry->va_intervals[entry->va_interval_count++];
                target_iv->va_base  = va_base;
                target_iv->gpa_base = va_base;
                target_iv->length   = va_len;
            } else {
                LAGFX_WARN("CmdMapMemoryImmediate: taskID=%u va_interval table "
                           "full (max=%u)", task_id, LAGFX_MAX_VA_INTERVALS);
            }
        }

        if (target_iv) {
            uint64_t gpa = 0, run_len = 0;
            bool ok = lagfx_task_translate_radix(p, task_id,
                                                  target_iv->va_base,
                                                  &gpa, &run_len);
            if (ok) {
                target_iv->gpa_base = gpa;
                LAGFX_LOG("CmdMapMemoryImmediate: taskID=%u translated "
                          "vaBase=0x%llx -> gpa=0x%llx (run=0x%llx)",
                          task_id,
                            (unsigned long long)target_iv->va_base,
                            (unsigned long long)gpa,
                            (unsigned long long)run_len);
            } else {
                LAGFX_WARN("CmdMapMemoryImmediate: taskID=%u vaBase=0x%llx "
                           "radix translate failed, keeping identity GPA",
                           task_id,
                           (unsigned long long)target_iv->va_base);
            }
        }
    }

    if (p->dev && p->dev->desc.shell.map_memory && entry->shell_task
        && va_len > 0) {
        enum { MAX_RANGES = 64 };
        lagfx_physical_range_t ranges[MAX_RANGES];
        uint64_t cursor = va_base;
        uint32_t ri = 0;
        while (cursor < va_base + va_len && ri < MAX_RANGES) {
            uint64_t gpa = 0, run = 0;
            bool ok = lagfx_task_translate_radix(p, task_id, cursor,
                                                  &gpa, &run);
            if (!ok) {
                LAGFX_WARN("CmdMapMemoryImmediate: taskID=%u scatter build "
                           "failed at va=0x%llx — stopping map_memory call",
                           task_id, (unsigned long long)cursor);
                ri = 0;
                break;
            }
            uint64_t remaining = (va_base + va_len) - cursor;
            if (run > remaining) {
                run = remaining;
            }
            ranges[ri].guest_physical_address = gpa;
            ranges[ri].length = run;
            ri++;
            cursor += run;
        }
        if (ri > 0) {
            bool mapped = p->dev->desc.shell.map_memory(
                p->dev->desc.shell.opaque,
                entry->shell_task,
                va_base,
                ranges,
                (size_t)ri,
                false);
            if (mapped) {
                LAGFX_LOG("CmdMapMemoryImmediate: taskID=%u mapped %u "
                          "scatter ranges into task VA window at "
                          "offset=0x%llx",
                          task_id, ri,
                          (unsigned long long)va_base);
            } else {
                LAGFX_WARN("CmdMapMemoryImmediate: taskID=%u "
                           "shell.map_memory returned false for %u ranges",
                           task_id, ri);
            }
        }
    }

    lagfx_resource_register(&p->resources, 0u,
                            LAGFX_RESOURCE_TYPE_BUFFER,
                            task_id, va_base, va_len);

    return LAGFX_HANDLER_OK;
}

/* ===========================================================================
 * CmdDeleteResource (0x08) — P2
 *
 * Payload layout UNKNOWN (min/max_payload=0 in the descriptor table).
 * Best-effort: read first 8 bytes as {u32 task_id, u32 ref} and
 * unregister from the resource registry. Fall back to {u32 ref} if
 * only 4 bytes available. Unknown payload is logged for RE.
 * =========================================================================== */

lagfx_handler_status_t lagfx_op_delete_resource(lagfx_protocol_t *p,
                                                 const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    if (!hdr->payload || hdr->payload_size < 4u) {
        return LAGFX_HANDLER_OK;
    }

    uint32_t word0 = lagfx_le32(hdr->payload + 0);

    if (hdr->payload_size >= 8u) {
        uint32_t word1 = lagfx_le32(hdr->payload + 4);
        lagfx_resource_unregister(&p->resources, word1, word0);
        LAGFX_TRACE("CmdDeleteResource: taskID=%u ref=0x%x stamp=0x%08x",
                   word0, word1, hdr->stamp);
    } else {
        LAGFX_TRACE("CmdDeleteResource: word0=0x%x stamp=0x%08x "
                   "(payload too short for task_id+ref pair)",
                   word0, hdr->stamp);
    }

    return LAGFX_HANDLER_OK;
}

/* ===========================================================================
 * CmdSetObjectAndPlacementList / CmdReleaseObjectReference (0x25) — P2
 *
 * Payload layout UNKNOWN. Best-effort: attempt to extract resource
 * references and unregister them. Wire format will be refined when
 * runtime captures are available.
 * =========================================================================== */

lagfx_handler_status_t lagfx_op_set_object_placement(lagfx_protocol_t *p,
                                                      const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    if (!hdr->payload || hdr->payload_size < 8u) {
        return LAGFX_HANDLER_OK;
    }

    uint32_t task_id = lagfx_le32(hdr->payload + 0);
    uint32_t count   = lagfx_le32(hdr->payload + 4);

    if (count == 0) {
        return LAGFX_HANDLER_OK;
    }

    if (count > ((uint32_t)hdr->payload_size - 8u) / 4u) {
        count = ((uint32_t)hdr->payload_size - 8u) / 4u;
    }

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t ref = lagfx_le32(hdr->payload + 8u + 4u * i);
        lagfx_resource_unregister(&p->resources, ref, task_id);
    }

    LAGFX_TRACE("CmdSetObjectPlacement: taskID=%u count=%u stamp=0x%08x",
               task_id, count, hdr->stamp);
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

/* ===========================================================================
 * CmdUnmapMemoryImmediate (kext opcode 0x22 on the Immediate vchan) — P1
 *
 * Same wire format as CmdMapMemoryImmediate (0x39): the kext places a
 * 20-byte trailer at the END of the payload:
 *
 *   [scatter blocks][20-byte trailer:
 *       +0x00  u32 task_id
 *       +0x04  u64 vaBase
 *       +0x0c  u64 vaLength
 *   ]
 *
 * The vchan drain loop dispatches opcode 0x22 here directly (no remap
 * to 0x03) so we read the trailer from the payload tail, not offset 0.
 * =========================================================================== */

lagfx_handler_status_t lagfx_op_unmap_memory_immediate(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    if (!hdr->payload || hdr->payload_size < 20u) {
        LAGFX_WARN("CmdUnmapMemoryImmediate: payload missing or too small "
                   "(size=%u, need >= 20)", (unsigned)hdr->payload_size);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    size_t off = (size_t)hdr->payload_size - 20u;
    uint32_t task_id        = lagfx_le32(hdr->payload + off + 0);
    uint64_t virtual_offset = (uint64_t)lagfx_le32(hdr->payload + off + 4)
                              | ((uint64_t)lagfx_le32(hdr->payload + off + 8) << 32);
    uint64_t length         = (uint64_t)lagfx_le32(hdr->payload + off + 12)
                              | ((uint64_t)lagfx_le32(hdr->payload + off + 16) << 32);

    if (off > 0) {
        size_t n = (off < 64u) ? off : 64u;
        char line[64 * 4 + 8];
        size_t lpos = 0;
        for (size_t i = 0; i < n; ++i) {
            int x = snprintf(line + lpos, sizeof(line) - lpos, "%02x ",
                             hdr->payload[i]);
            if (x <= 0 || (size_t)x >= sizeof(line) - lpos) break;
            lpos += (size_t)x;
        }
        LAGFX_TRACE("CmdUnmapMemoryImmediate: scatter prefix[%zu]: %s",
                  off, line);
    }

    LAGFX_LOG("CmdUnmapMemoryImmediate: taskID=%u vaBase=0x%llx "
              "vaLength=0x%llx stamp=0x%08x",
              task_id, (unsigned long long)virtual_offset,
              (unsigned long long)length, hdr->stamp);

    lagfx_task_entry_t *task = lagfx_protocol_find_task(p, task_id);
    if (!task) {
        LAGFX_WARN("CmdUnmapMemoryImmediate: taskID=%u not found "
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
            LAGFX_WARN("CmdUnmapMemoryImmediate: shell.unmap_memory returned false "
                       "for taskID=%u vm_off=0x%llx length=%llu "
                       "(completing stamp anyway — fail-open)",
                       task_id, (unsigned long long)virtual_offset,
                       (unsigned long long)length);
            status = LAGFX_HANDLER_ERR_STATE;
        }
    } else {
        LAGFX_WARN("CmdUnmapMemoryImmediate: no shell.unmap_memory callback; "
                   "taskID=%u treated as success (scaffold)", task_id);
    }

    return status;
}

/*
 * libapplegfx-vulkan — Root channel dispatcher (ch 0)
 * src/dispatchers/channel_0_dispatcher.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Drains the root command ring on a BAR0+0x1008 doorbell. read_ptr /
 * write_ptr are BYTE OFFSETS within the ring (not command counts);
 * each command's 12-byte header is laid out on the wire as:
 *
 *   off 0: u16 opcode
 *   off 2: u16 arg_count_8b
 *   off 4: u32 length         (total bytes, header + payload)
 *   off 8: u32 stamp
 *
 * payload (length - 12 bytes) follows the header inline in the ring.
 * Wrap is handled with a two-DMA read when a command straddles
 * ring_size. See the legacy lagfx_fifo_drain in
 * git show af87e8c~1:src/protocol/fifo.c — this is the same shape,
 * adapted to the new lagfx_protocol_t layout and handler tables.
 */

#include "channel_0_dispatcher.h"
#include "../device.h"
#include "../doorbell.h"
#include "../common/le.h"
#include "../common/log.h"
#include "protocol/opcodes.h"
#include "protocol/state.h"
#include "handlers/handlers.h"
#include "handlers/iosurface/iosurface.h"

#include <stdio.h>
#include <string.h>

/* Sanity cap matches the legacy drainer. 4 KiB per command is well
 * above any single legitimate macOS command we've seen. */
#define LAGFX_CH0_DRAIN_MAX_CMDS 128u
#define LAGFX_CH0_MAX_CMD_BYTES  4096u

/* === Inline helpers for ch0 extended opcodes (kext-only namespace) ===
 *
 * These three opcodes (0x30, 0x33, 0x38) are the kext's initial setup
 * burst — without responses the kext never publishes
 * AppleParavirtGPUControl into ioreg. Wire formats and semantics
 * recovered from the pre-refactor `lagfx_op_*` family in
 * `git show af87e8c~1:src/protocol/ops_device.c` and `ops_queue.c`,
 * adapted to the current `lagfx_protocol_t` field layout.
 *
 * Per reference_lagfx_mmio_handler.md rule 2: these are inline in the
 * switch arm because the live readback verifying them is what we're
 * about to do (the inflight ioreg test). If 0x30/0x33/0x38 prove out
 * end-to-end after this batch, they can promote to named helpers.
 */

/* Dispatch a single command to the appropriate handler. Handlers
 * return a status; on OK (or fail-open SIZE/STATE errors) we still
 * raise the stamp so the guest doesn't park forever in waitForStamp.
 */
static void dispatch_command(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return;
    }

    LAGFX_TRACE("ch0 dispatch: opcode=0x%04x len=%u stamp=0x%08x",
                hdr->opcode, hdr->length, hdr->stamp);

    switch (hdr->opcode) {
        /* Task management */
        case LAGFX_OP_DEFINE_TASK2:
            lagfx_task_define_task2(p, hdr);
            break;
        case LAGFX_OP_DELETE_TASK:
            lagfx_task_delete_task(p, hdr);
            break;

        /* Memory mapping */
        case LAGFX_OP_MAP_MEMORY2:
            lagfx_memory_map_memory2(p, hdr);
            break;
        case LAGFX_OP_UNMAP_MEMORY:
            lagfx_memory_unmap_memory(p, hdr);
            break;

        /* Device info queries */
        case LAGFX_OP_GET_DEVICE_INFO:
            lagfx_op_get_device_info(p, hdr);
            break;
        case LAGFX_OP_GET_DEVICE_INFO_2:
            lagfx_op_get_device_info_2(p, hdr);
            break;

        /* Debug/NOP */
        case LAGFX_OP_NOP:
            lagfx_util_nop(p, hdr);
            break;
        case LAGFX_OP_DEBUG:
            lagfx_op_debug(p, hdr);
            break;

        /* CmdExecIndirect2 (0x20) — dylib emits this on the root
         * channel (kext-side per-channel variant is 0x37 on ch 1..4).
         * Same outer payload format per M4-inner-opcode-implementation-
         * guide.md §1.1. Stage 20 inner walker lives in the compute
         * exec handler — reuse it. */
        case LAGFX_OP_EXEC_INDIRECT2:  /* 0x20 */
            lagfx_compute_exec_cmdbuf(p, hdr);
            break;

        /* === IOSurface family (0x26-0x2a) ============================
         * RE: PROTOCOL.md §14.5; pre-refactor src/protocol/ops_iosurface.c
         * at b652199~1. The kext / dylib emit these on the root channel
         * for IOSurface lifecycle. Real backing-VkImage allocation is
         * Stage 30 work; for now we maintain the resource_registry
         * mapping so cross-task lookup / import / unmap stay coherent. */
        case LAGFX_OP_DELETE_IOSURFACE_BACKING:   /* 0x26 */
            lagfx_iosurface_delete_backing2(p, hdr);
            break;
        case LAGFX_OP_IOSURFACE_CREATE:           /* 0x27 */
            lagfx_iosurface_create_backing2(p, hdr);
            break;
        case LAGFX_OP_IOSURFACE_LOOKUP:           /* 0x28 */
            lagfx_iosurface_lookup(p, hdr);
            break;
        case LAGFX_OP_IOSURFACE_UPDATE:           /* 0x29 — CmdImportIOSurfaceMachPort */
            lagfx_iosurface_import_mach_port(p, hdr);
            break;
        case LAGFX_OP_IOSURFACE_UNMAP:            /* 0x2a */
            lagfx_iosurface_unmap(p, hdr);
            break;

        /* === Kext extended opcodes (0x30..0x3a) =====================
         * The kext fires four 0x30 + one 0x33 + one 0x38 in its
         * initial setup burst. Without these responses
         * AppleParavirtGPUControl never publishes into ioreg. */

        case LAGFX_OP_DEFINE_CHILD_CHANNEL: {
            /* CmdDefineChildChannel (0x30) — kext-side child-FIFO
             * registration. Live wire shows 4-byte payload (bare
             * u32 child_id at +0), same shape as 0x04
             * CmdDefineChildFIFO. The pre-refactor opcodes.c
             * comment claimed "0x400-byte payload, child_id at +4"
             * was based on a stale observation; current macOS
             * 15.7.5 emits the compact form. Ring geometry comes
             * from MMIO setters, not the payload. */
            if (!hdr->payload || hdr->payload_size < 4u) {
                LAGFX_WARN("CmdDefineChildChannel: payload too small (%u)",
                           (unsigned)hdr->payload_size);
                break;
            }
            uint32_t child_id = lagfx_le32(hdr->payload + 0);
            lagfx_fifo_entry_t *entry = lagfx_protocol_find_fifo(p, child_id);
            if (entry) {
                LAGFX_LOG("CmdDefineChildChannel: child_id=%u (re-using slot) stamp=0x%08x",
                          child_id, hdr->stamp);
            } else {
                entry = lagfx_protocol_alloc_fifo_slot(p);
                if (!entry) {
                    LAGFX_WARN("CmdDefineChildChannel: fifo table full (max=%u)",
                               LAGFX_MAX_FIFOS);
                    break;
                }
                entry->id   = child_id;
                entry->live = true;
                LAGFX_LOG("CmdDefineChildChannel: child_id=%u registered stamp=0x%08x",
                          child_id, hdr->stamp);
            }
            break;
        }

        case LAGFX_OP_SET_RESOURCE_HEAP: {
            /* CmdSetResourceHeap (0x33) — pre-refactor ops_device.c
             * lagfx_op_set_resource_heap. 12-byte payload:
             *   +0  u32 task_id
             *   +4  u32 heap_pfn
             *   +8  u32 heap_size
             * Records the heap hint onto the task entry so later
             * resource lookups can locate the kext-side heap. The
             * task may not yet be defined (CmdDefineHostTask
             * sometimes lands AFTER this); fail-open in that case. */
            if (!hdr->payload || hdr->payload_size < 12u) {
                LAGFX_WARN("CmdSetResourceHeap: payload too small (%u)",
                           (unsigned)hdr->payload_size);
                break;
            }
            uint32_t task_id   = lagfx_le32(hdr->payload + 0);
            uint32_t heap_pfn  = lagfx_le32(hdr->payload + 4);
            uint32_t heap_size = lagfx_le32(hdr->payload + 8);
            lagfx_task_entry_t *entry = lagfx_protocol_find_task(p, task_id);
            if (entry) {
                entry->heap_pfn  = heap_pfn;
                entry->heap_size = heap_size;
                LAGFX_LOG("CmdSetResourceHeap: taskID=%u heap_pfn=0x%x heap_size=0x%x stamp=0x%08x",
                          task_id, heap_pfn, heap_size, hdr->stamp);
            } else {
                LAGFX_LOG("CmdSetResourceHeap: taskID=%u not found "
                          "(deferred; heap_pfn=0x%x heap_size=0x%x) stamp=0x%08x",
                          task_id, heap_pfn, heap_size, hdr->stamp);
            }
            break;
        }

        /* === Core opcodes carried by the pre-refactor opcodes.c
         *     table but never observed on the live wire today. Logged
         *     + acked so a future RE pass sees them in /tmp/lagfx.log
         *     rather than via the "unknown opcode" channel. */

        case LAGFX_OP_DEFINE_CHILD_FIFO:     /* 0x04 — superseded by ch-side 0x30 */
            LAGFX_LOG("ch0: 0x04 CmdDefineChildFIFO stamp=0x%08x payload=%u (dylib-side; log-ack)",
                      hdr->stamp, (unsigned)hdr->payload_size);
            break;
        case LAGFX_OP_DELETE_CHILD_FIFO:     /* 0x05 */
            LAGFX_LOG("ch0: 0x05 CmdDeleteChildFIFO stamp=0x%08x payload=%u",
                      hdr->stamp, (unsigned)hdr->payload_size);
            break;
        case LAGFX_OP_INVALIDATE_RESOURCES:  /* 0x06 */
            LAGFX_LOG("ch0: 0x06 CmdInvalidateResources stamp=0x%08x payload=%u",
                      hdr->stamp, (unsigned)hdr->payload_size);
            break;
        case LAGFX_OP_DISCARD_RESOURCES:     /* 0x07 */
            LAGFX_LOG("ch0: 0x07 CmdDiscardResources stamp=0x%08x payload=%u",
                      hdr->stamp, (unsigned)hdr->payload_size);
            break;
        case LAGFX_OP_DELETE_RESOURCE:       /* 0x08 */
            LAGFX_LOG("ch0: 0x08 CmdDeleteResource stamp=0x%08x payload=%u",
                      hdr->stamp, (unsigned)hdr->payload_size);
            break;
        case LAGFX_OP_REPLACE_PHYSICAL:      /* 0x09 */
            LAGFX_LOG("ch0: 0x09 CmdReplacePhysical stamp=0x%08x payload=%u",
                      hdr->stamp, (unsigned)hdr->payload_size);
            break;
        case LAGFX_OP_GET_COMPUTE_INFO:      /* 0x0b */
            LAGFX_LOG("ch0: 0x0b CmdGetComputeInfo stamp=0x%08x payload=%u "
                      "(TODO: Stage 30 reply table)",
                      hdr->stamp, (unsigned)hdr->payload_size);
            break;
        case LAGFX_OP_DELAY:                 /* 0x0c */
            LAGFX_LOG("ch0: 0x0c CmdDelay stamp=0x%08x payload=%u",
                      hdr->stamp, (unsigned)hdr->payload_size);
            break;
        case LAGFX_OP_EXEC_INDIRECT3:        /* 0x21 — newer variant of 0x20 */
            LAGFX_LOG("ch0: 0x21 CmdExecIndirect3 stamp=0x%08x payload=%u "
                      "(TODO: Stage 30 dispatch via compute_exec)",
                      hdr->stamp, (unsigned)hdr->payload_size);
            break;
        case LAGFX_OP_SYNCHRONIZE_RESOURCES: /* 0x42 */
            LAGFX_LOG("ch0: 0x42 CmdSynchronizeResources stamp=0x%08x payload=%u",
                      hdr->stamp, (unsigned)hdr->payload_size);
            break;
        case LAGFX_OP_SYNCHRONIZE_DISCARD:   /* 0x23 */
            LAGFX_LOG("ch0: 0x23 CmdSynchronizeDiscard stamp=0x%08x payload=%u",
                      hdr->stamp, (unsigned)hdr->payload_size);
            break;
        case LAGFX_OP_HEAP_TEX_SIZE_ALIGN:   /* 0x80 */
            LAGFX_LOG("ch0: 0x80 CmdHeapTexSizeAndAlign stamp=0x%08x payload=%u "
                      "(TODO: Stage 30 reply via info_replies analogue)",
                      hdr->stamp, (unsigned)hdr->payload_size);
            break;
        case LAGFX_OP_RESET_RASTERIZATION_RATE: /* 0x81 */
            LAGFX_LOG("ch0: 0x81 CmdResetRasterizationRate stamp=0x%08x payload=%u",
                      hdr->stamp, (unsigned)hdr->payload_size);
            break;
        case LAGFX_OP_DELETE_SHARED_TEX_BACK:  /* 0x82 */
            LAGFX_LOG("ch0: 0x82 CmdDeleteSharedTextureBacking stamp=0x%08x payload=%u",
                      hdr->stamp, (unsigned)hdr->payload_size);
            break;
        case LAGFX_OP_FREE_VIRTUAL_CHANNEL:  /* 0x31 — pairs 0x30 */
            LAGFX_LOG("ch0: 0x31 CmdFreeVirtualChannel stamp=0x%08x payload=%u",
                      hdr->stamp, (unsigned)hdr->payload_size);
            break;
        case LAGFX_OP_NEW_USER_CLIENT:       /* 0x3b */
            LAGFX_LOG("ch0: 0x3b CmdNewUserClient stamp=0x%08x payload=%u",
                      hdr->stamp, (unsigned)hdr->payload_size);
            break;
        case LAGFX_OP_UNKNOWN_3C:            /* 0x3c — outer variant (RE pending) */
            LAGFX_LOG("ch0: 0x3c CmdUnknown3C stamp=0x%08x payload=%u (RE pending)",
                      hdr->stamp, (unsigned)hdr->payload_size);
            break;
        case LAGFX_OP_EXEC_INDIRECT_EXT_41:  /* 0x41 */
            LAGFX_LOG("ch0: 0x41 CmdExecIndirectExt41 stamp=0x%08x payload=%u",
                      hdr->stamp, (unsigned)hdr->payload_size);
            break;

        case LAGFX_OP_DEFINE_HOST_TASK: {
            /* CmdDefineHostTask (0x38) — pre-refactor ops_device.c
             * lagfx_op_define_host_task. 16-byte payload:
             *   +0  u32 slot_index    (task_id = slot_index >> 1)
             *   +4  u32 reserved      (handleHint low half)
             *   +8  u32 flags         (handleHint high half)
             *   +12 u32 root_page_pfn
             *
             * Allocates/re-uses a task entry, stamps root_page_pfn
             * onto it. This is the page-table-base anchor the host
             * needs to walk the 3-level radix for VA→GPA on this
             * task's resources. */
            if (!hdr->payload || hdr->payload_size < 16u) {
                LAGFX_WARN("CmdDefineHostTask: payload too small (%u)",
                           (unsigned)hdr->payload_size);
                break;
            }
            uint32_t slot_index    = lagfx_le32(hdr->payload + 0);
            uint32_t root_page_pfn = lagfx_le32(hdr->payload + 12);
            uint32_t task_id       = slot_index >> 1;

            lagfx_task_entry_t *entry = lagfx_protocol_find_task(p, task_id);
            if (!entry) {
                entry = lagfx_protocol_alloc_task_slot(p);
                if (!entry) {
                    LAGFX_WARN("CmdDefineHostTask: task table full (max=%u)",
                               LAGFX_MAX_TASKS);
                    break;
                }
                entry->id   = task_id;
                entry->live = true;
            }
            entry->root_page_pfn = (uint64_t)root_page_pfn;
            LAGFX_LOG("CmdDefineHostTask: taskID=%u root_page_pfn=0x%x stamp=0x%08x",
                      task_id, root_page_pfn, hdr->stamp);
            break;
        }

        default:
            LAGFX_WARN("ch0 dispatch: unknown opcode 0x%04x stamp=0x%08x",
                       hdr->opcode, hdr->stamp);
            p->unknown_opcode_count++;
            break;
    }
}

/* Drain the root channel ring. Returns the number of commands
 * processed. read_ptr and write_ptr are byte offsets into the ring;
 * each command advances read_ptr by hdr.length, wrapping at
 * ring_size. Each completion raises the slot-0 stamp + MSI so the
 * kext's waitForStamp() can return.
 */
size_t channel_0_dispatcher_drain(lagfx_protocol_t *p) {
    if (!lagfx_protocol_is_valid(p)) {
        return 0;
    }

    if (!p->ring_armed || p->ring_size == 0u || p->ring_base_gpa == 0u) {
        LAGFX_TRACE("ch0 drain: ring not armed (armed=%d size=0x%x gpa=0x%llx)",
                    (int)p->ring_armed, p->ring_size,
                    (unsigned long long)p->ring_base_gpa);
        return 0;
    }

    lagfx_device_t *dev = (lagfx_device_t *)p->dev;
    if (!dev || !dev->desc.shell.read_memory) {
        LAGFX_WARN("ch0 drain: no shell.read_memory callback");
        return 0;
    }

    uint32_t write_ptr = p->write_ptr;
    uint32_t ring_size = p->ring_size;
    uint64_t ring_base = p->ring_base_gpa;

    if (p->read_ptr == write_ptr) {
        LAGFX_TRACE("ch0 drain: caught up rp=0x%x wp=0x%x", p->read_ptr, write_ptr);
        return 0;
    }

    LAGFX_LOG("ch0 drain: rp=0x%x wp=0x%x ring_size=0x%x base_gpa=0x%llx",
              p->read_ptr, write_ptr, ring_size,
              (unsigned long long)ring_base);

    size_t cmds = 0;
    for (unsigned i = 0; i < LAGFX_CH0_DRAIN_MAX_CMDS; ++i) {
        if (p->read_ptr == write_ptr) {
            break;  /* caught up */
        }

        uint32_t rp = p->read_ptr % ring_size;
        uint64_t hdr_gpa = ring_base + (uint64_t)rp;

        /* Step 1: read the 12-byte header. The header itself never
         * wraps because rings are aligned and headers are 12 bytes;
         * a wrap mid-header would mean the producer wrote a
         * malformed entry. We still defensively wrap-read the body
         * below. */
        uint8_t hdr_buf[LAGFX_CMD_HEADER_BYTES];
        if (!dev->desc.shell.read_memory(dev->desc.shell.opaque,
                                          hdr_gpa,
                                          LAGFX_CMD_HEADER_BYTES,
                                          hdr_buf)) {
            LAGFX_WARN("ch0 drain: header DMA failed at gpa=0x%llx",
                       (unsigned long long)hdr_gpa);
            break;
        }

        uint16_t opcode       = lagfx_le16(hdr_buf + 0);
        uint16_t arg_count_8b = lagfx_le16(hdr_buf + 2);
        uint32_t length       = lagfx_le32(hdr_buf + 4);
        uint32_t stamp        = lagfx_le32(hdr_buf + 8);

        if (length < LAGFX_CMD_HEADER_BYTES ||
            length > LAGFX_CH0_MAX_CMD_BYTES ||
            length > ring_size) {
            LAGFX_WARN("ch0 drain: bad length 0x%x at rp=0x%x opcode=0x%04x — stop",
                       length, rp, opcode);
            break;
        }

        /* Step 2: read the full command (header + payload) into a
         * local buffer so handlers can chase payload->payload_size.
         * Handles wrap by a second DMA for the tail. */
        uint8_t cmd_buf[LAGFX_CH0_MAX_CMD_BYTES];
        uint32_t head_len = ring_size - rp;
        if (head_len >= length) {
            if (!dev->desc.shell.read_memory(dev->desc.shell.opaque,
                                              hdr_gpa, length, cmd_buf)) {
                LAGFX_WARN("ch0 drain: body DMA failed at gpa=0x%llx len=%u",
                           (unsigned long long)hdr_gpa, length);
                break;
            }
        } else {
            if (!dev->desc.shell.read_memory(dev->desc.shell.opaque,
                                              hdr_gpa, head_len, cmd_buf)) {
                LAGFX_WARN("ch0 drain: wrapped head DMA failed");
                break;
            }
            if (!dev->desc.shell.read_memory(dev->desc.shell.opaque,
                                              ring_base,
                                              length - head_len,
                                              cmd_buf + head_len)) {
                LAGFX_WARN("ch0 drain: wrapped tail DMA failed");
                break;
            }
        }

        /* Build derived header. payload pointer is just past the
         * 12-byte on-wire header; payload_size is length - 12. */
        lagfx_cmd_header_t hdr;
        hdr.opcode       = opcode;
        hdr.arg_count_8b = arg_count_8b;
        hdr.length       = length;
        hdr.stamp        = stamp;
        hdr.payload_size = (uint16_t)((length > LAGFX_CMD_HEADER_BYTES)
                                       ? (length - LAGFX_CMD_HEADER_BYTES)
                                       : 0u);
        hdr.payload      = (hdr.payload_size > 0)
                              ? (cmd_buf + LAGFX_CMD_HEADER_BYTES)
                              : NULL;

        /* Hex dump of first 32 bytes on TRACE for bring-up debugging. */
        if (lagfx_log_level() >= LAGFX_LOG_LVL_TRACE) {
            uint32_t dump_len = length < 32u ? length : 32u;
            char hexline[128];
            size_t pos = 0;
            for (uint32_t j = 0; j < dump_len && pos + 4 < sizeof(hexline); ++j) {
                int n = snprintf(hexline + pos, sizeof(hexline) - pos,
                                 "%02x ", cmd_buf[j]);
                if (n < 0 || (size_t)n >= sizeof(hexline) - pos) break;
                pos += (size_t)n;
            }
            hexline[pos] = '\0';
            LAGFX_TRACE("ch0 drain: bytes[%u] %s", dump_len, hexline);
        }

        dispatch_command(p, &hdr);

        /* Every root-channel command unconditionally signals its
         * stamp on completion (see paravirt-re
         * waitForStamp-mechanism-summary.md). */
        lagfx_protocol_complete_stamp(p, stamp);

        p->total_cmds_seen++;
        cmds++;
        p->read_ptr = (rp + length) % ring_size;
    }

    LAGFX_LOG("ch0 drain: drained=%zu new rp=0x%x", cmds, p->read_ptr);
    return cmds;
}

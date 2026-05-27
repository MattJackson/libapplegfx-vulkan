/*
 * libapplegfx-vulkan — Compute channel dispatcher (ch 1-4)
 * src/dispatchers/channel_compute_dispatcher.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Per-channel ring drain for compute vchans. Each child channel
 * (chan_id 1..4) has its own command ring registered via
 * FIFORingDescriptor at:
 *
 *   shared_control + 0x400 + 20 * (chan_id - 1)
 *
 * where shared_control is the page registered at BAR0+0x101c
 * (ring_shared_page_pfn). See PROTOCOL.md §3, §5 and
 * paravirt-re/library/state-machines/FIFORingDescriptor.md (the
 * authoritative reference verified 2026-04-25 against a live xp dump).
 *
 * Wire format and read semantics on each ring mirror the root-channel
 * drainer (channel_0_dispatcher.c). The descriptor's read_ptr is
 * authoritative — host advances it after draining.
 */

#include "channel_compute_dispatcher.h"
#include "ring_common.h"
#include "../doorbell.h"
#include "../device.h"
#include "../common/le.h"
#include "../common/log.h"
#include "protocol/opcodes.h"
#include "protocol/state.h"
#include "handlers/handlers.h"
#include "handlers/iosurface/iosurface.h"

#include <stddef.h>

#define LAGFX_COMPUTE_DRAIN_MAX_CMDS 128u
#define LAGFX_COMPUTE_MAX_CMD_BYTES  4096u

/* VirtualChannel ring size per FIFORingDescriptor.md (§"ring size"):
 * 0x10000 (64 KiB). See ring_common.h for the descriptor read +
 * PFN-array resolve helpers.
 */
#define LAGFX_COMPUTE_RING_SIZE  0x10000u

/* Dispatch a single command to appropriate handler. */
static void dispatch_command(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return;
    }

    LAGFX_TRACE("compute dispatch: opcode=0x%04x stamp=0x%08x", hdr->opcode, hdr->stamp);

    switch (hdr->opcode) {
        /* CmdExecIndirect2 on a compute channel — the inner stream is
         * where Metal commands live. See render-decoder-handlers.md. */
        case LAGFX_OP_EXEC_INDIRECT2:  /* 0x20 */
            lagfx_compute_exec_cmdbuf(p, hdr);
            break;

        /* Kext-side per-channel exec variant per
         * M4-inner-opcode-implementation-guide.md §1.1 — same outer
         * payload as 0x20, kext emits this on the per-channel exec
         * rings (ch 1..4). Conflicts with LAGFX_OP_CHANNEL_EVENT_37
         * naming in opcodes.h — the kext-disasm pass classified 0x37
         * as "ChannelEventMachine-adjacent" before the M4 RE pass
         * identified it as the per-channel CmdExecIndirect2. The
         * payload's outer layout (12 + dc*24 + rc*16) distinguishes
         * the two at runtime; for now route 0x37 to exec_cmdbuf
         * which handles both empty (event-only) and populated (real
         * exec) payloads gracefully. */
        case LAGFX_OP_CHANNEL_EVENT_37:  /* 0x37 — see comment above */
            lagfx_compute_exec_cmdbuf(p, hdr);
            break;

        /* Channel-event / immediate-vchan opcodes fired by the kext
         * on the Immediate (ch=2) and Uploads/Downloads (ch=3/4)
         * channels. Log + ack — these don't carry render workloads
         * (no inner-PGCmdHeader stream); they're kext-internal lifecycle
         * events. Documented in paravirt-re/library/PROTOCOL.md §13
         * + journey/opcodes-0x35-0x36-0x39.md. */
        case LAGFX_OP_UNMAP_MEMORY_IMMEDIATE: /* 0x22 — CmdUnmapMemoryImmediate */
            LAGFX_LOG("compute: 0x22 CmdUnmapMemoryImmediate ch=%u stamp=0x%08x "
                      "payload_size=%u",
                      (unsigned)p->current_chan_id, hdr->stamp,
                      (unsigned)hdr->payload_size);
            break;
        case LAGFX_OP_SET_OBJECT_LIST:        /* 0x24 CmdSetObjectList */
            /* Stub. Payload per command-buffer-format.md:254 is
             * `objectArray[], count`. Phase 6b will parse the
             * objectArray (APVObjectEntry records) to register
             * pipeline-state metallib bytes. Today: log+ack discards
             * the payload. */
            LAGFX_LOG("compute: 0x24 CmdSetObjectList ch=%u stamp=0x%08x payload_size=%u",
                      (unsigned)p->current_chan_id, hdr->stamp,
                      (unsigned)hdr->payload_size);
            break;
        case LAGFX_OP_SET_OBJECT_PLACEMENT:   /* 0x25 CmdSetObjectAndPlacementList */
            /* MISLABELED enum: 0x25 is `CmdSetObjectAndPlacementList`,
             * NOT just "object placement" alone — it carries BOTH the
             * objectArray AND the placementArray (command-buffer-
             * format.md:255). This is the canonical path APVObjectEntry
             * records arrive over (ENTRY-007). Stub today; Phase 6b
             * will decode the dual-array payload + register the
             * resulting object → metallib bindings in a per-task
             * registry. */
            LAGFX_LOG("compute: 0x25 CmdSetObjectAndPlacementList ch=%u stamp=0x%08x payload_size=%u",
                      (unsigned)p->current_chan_id, hdr->stamp,
                      (unsigned)hdr->payload_size);
            break;
        case LAGFX_OP_IOSURFACE_LOOKUP:       /* 0x28 */
            /* Live evidence shows 0x28 on compute channels too — route
             * to the real lookup handler so resource_registry stays
             * in sync with the cross-task IOSurface lifecycle. */
            lagfx_iosurface_lookup(p, hdr);
            break;
        case LAGFX_OP_CHANNEL_EVENT_34:       /* 0x34 */
        case LAGFX_OP_CHANNEL_EVENT_35:       /* 0x35 */
        case LAGFX_OP_CHANNEL_EVENT_36:       /* 0x36 */
            /* ChannelEventMachine-adjacent (kext-internal scheduler
             * pings). Log + ack. 0x37 is handled above as the
             * per-channel CmdExecIndirect2. */
            LAGFX_LOG("compute: channel_event 0x%02x ch=%u stamp=0x%08x payload_size=%u",
                      (unsigned)hdr->opcode, (unsigned)p->current_chan_id,
                      hdr->stamp, (unsigned)hdr->payload_size);
            break;
        case LAGFX_OP_MAP_MEMORY_IMMEDIATE:   /* 0x39 — CmdMapMemoryImmediate */
            LAGFX_LOG("compute: 0x39 CmdMapMemoryImmediate ch=%u stamp=0x%08x payload_size=%u",
                      (unsigned)p->current_chan_id, hdr->stamp,
                      (unsigned)hdr->payload_size);
            break;

        case LAGFX_OP_VCHAN_REPLACE_PHYSICAL: { /* 0x3c — per-resource replacePhysical on Immediate vchan */
            /* RE: paravirt-re — single kext emit-site at vaddr 0x14564494
             * in AppleParavirtResource::replacePhysical() (vaddr 0x145643f2,
             * mangled __ZN21AppleParavirtResource15replacePhysicalEv).
             * See paravirt-re/classes/AppleParavirtResource.md and the
             * caller annotated/AppleParavirtMemoryMap-commitIntoGPUPageTable.annotated.asm
             * (loop body at 0x1455dc4e calls this for each resource in
             * task[+0xa0] when [chan+0x114] re-binding flag is set).
             *
             * Wire trailer: 8 bytes `{u32 eventID/taskID, u32 counter}` —
             * same emit-helper family as 0x25/0x36 (single
             * apvgpu_cmd_builder_reserve(8) after channel lock, then
             * `[r12]=eventID; [r12+4]=[chan+0x180]=counter`). Total
             * command size = 12 (header) + 8 (trailer) = 20 bytes;
             * matches the live ring delta `(wp - rp) == 0x14` observed
             * in /tmp/lagfx.log compute-drain traces.
             *
             * Semantics: kext notifies host that a per-task Resource has
             * had its host backing pages relocated and any host-side
             * mapping caches should be invalidated. The downstream side-
             * table publishes (two `vt[+0x1b8]` calls at kext 0x14564598 /
             * 0x145645b6) update `[accel_shared+0x380]`'s per-task tracker
             * with the new task[+0x20] / task[+0x270] pointers — these
             * are kext-internal cleanup, not wire data we need to
             * consume.
             *
             * Log+ack correct because: until lagfx allocates real
             * VkImage/VkBuffer per-resource (Stage 30 work; the
             * pre-refactor real-translation paths in dead-code-to-revive/
             * have been ported as parse-and-trace stubs per
             * project_m5_inflight.md), there is no host-side mapping
             * cache to invalidate. The kext does NOT wait on a
             * 0x3c-specific stamp completion — `stamp` is the standard
             * per-channel monotonic that the universal complete_stamp
             * path already raises after dispatch.
             *
             * TODO (Stage 30): when per-resource Vulkan backing lands,
             * look up the resource by `eventID` (== task->id via
             * 0x14563376) and flush any host-side mapping cache. The
             * `counter` field is the per-channel emit counter and is
             * not load-bearing for host behaviour. */
            uint32_t eventID = 0, counter = 0;
            if (hdr->payload && hdr->payload_size >= 8) {
                eventID = lagfx_le32(hdr->payload + 0);
                counter = lagfx_le32(hdr->payload + 4);
            }
            LAGFX_LOG("compute: 0x3c VchanReplacePhysical ch=%u stamp=0x%08x "
                      "eventID=%u counter=%u payload_size=%u",
                      (unsigned)p->current_chan_id, hdr->stamp,
                      eventID, counter, (unsigned)hdr->payload_size);
            break;
        }

        default:
            LAGFX_WARN("compute dispatch: ch=%u unknown opcode 0x%04x stamp=0x%08x",
                       (unsigned)p->current_chan_id, hdr->opcode, hdr->stamp);
            p->unknown_opcode_count++;
            break;
    }
}

/* Drain ring buffer for compute channels. Returns number of commands processed. */
size_t channel_compute_dispatcher_drain(lagfx_protocol_t *p, uint32_t chan_id) {
    if (!lagfx_protocol_is_valid(p)) {
        return 0;
    }

    /* Look up the per-channel ring geometry from the FIFORingDescriptor.
     * Helper lives in ring_common.{c,h}; shared with display dispatcher. */
    uint64_t desc_gpa = 0;
    uint32_t write_ptr = 0;
    uint32_t read_ptr = 0;
    uint64_t page0_gpa = 0;
    if (!lagfx_ring_fifo_descriptor_read(p, chan_id, &desc_gpa, &write_ptr,
                                          &read_ptr, &page0_gpa)) {
        return 0;
    }

    uint32_t ring_size = LAGFX_COMPUTE_RING_SIZE;

    if (write_ptr == read_ptr) {
        LAGFX_TRACE("compute drain: ch=%u caught up rp=0x%x wp=0x%x",
                    chan_id, read_ptr, write_ptr);
        return 0;
    }

    LAGFX_LOG("compute drain: ch=%u desc_gpa=0x%llx page0_gpa=0x%llx "
              "ring_size=0x%x rp=0x%x wp=0x%x",
              chan_id,
              (unsigned long long)desc_gpa,
              (unsigned long long)page0_gpa,
              ring_size, read_ptr, write_ptr);

    lagfx_device_t *dev = (lagfx_device_t *)p->dev;
    if (!dev || !dev->desc.shell.read_memory) {
        LAGFX_WARN("compute drain: no shell.read_memory callback");
        return 0;
    }

    uint32_t rp = read_ptr;
    size_t cmds = 0;
    for (unsigned i = 0; i < LAGFX_COMPUTE_DRAIN_MAX_CMDS; ++i) {
        if (rp == write_ptr) break;

        /* Resolve command-bytes GPA via page0's PFN-array. */
        uint64_t hdr_gpa = 0;
        if (!lagfx_ring_resolve_data_gpa(p, page0_gpa, ring_size, rp, &hdr_gpa)) {
            break;
        }

        uint8_t hdr_buf[LAGFX_CMD_HEADER_BYTES];
        if (!dev->desc.shell.read_memory(dev->desc.shell.opaque,
                                          hdr_gpa,
                                          LAGFX_CMD_HEADER_BYTES,
                                          hdr_buf)) {
            LAGFX_WARN("compute drain: ch=%u header DMA failed at gpa=0x%llx",
                       chan_id, (unsigned long long)hdr_gpa);
            break;
        }

        uint16_t opcode       = lagfx_le16(hdr_buf + 0);
        uint16_t arg_count_8b = lagfx_le16(hdr_buf + 2);
        uint32_t length       = lagfx_le32(hdr_buf + 4);
        uint32_t stamp        = lagfx_le32(hdr_buf + 8);

        if (length < LAGFX_CMD_HEADER_BYTES ||
            length > LAGFX_COMPUTE_MAX_CMD_BYTES ||
            length > ring_size) {
            LAGFX_WARN("compute drain: ch=%u bad length 0x%x at rp=0x%x "
                       "opcode=0x%04x first8=%02x%02x%02x%02x%02x%02x%02x%02x — stop",
                       chan_id, length, rp, opcode,
                       hdr_buf[0], hdr_buf[1], hdr_buf[2], hdr_buf[3],
                       hdr_buf[4], hdr_buf[5], hdr_buf[6], hdr_buf[7]);
            break;
        }

        /* Read body. For compute (64 KiB ring) a command can straddle
         * a page boundary within the same ring slot — resolve each
         * 4 KiB chunk independently because page0[i] may map to
         * non-contiguous physical pages.
         *
         * Body lands in the protocol's compute scratch (state.h's
         * "Per-dispatcher scratch buffers"). BQL-serialised. */
        uint8_t *cmd_buf = p->scratch_compute;
        bool body_ok = true;
        uint32_t bytes_read = 0;
        while (bytes_read < length) {
            uint64_t chunk_gpa = 0;
            if (!lagfx_ring_resolve_data_gpa(p, page0_gpa, ring_size,
                                        rp + bytes_read, &chunk_gpa)) {
                body_ok = false;
                break;
            }
            uint32_t chunk_off_in_page = (uint32_t)(chunk_gpa & 0xfffu);
            uint32_t chunk_len = 0x1000u - chunk_off_in_page;
            if (chunk_len > length - bytes_read) {
                chunk_len = length - bytes_read;
            }
            if (!dev->desc.shell.read_memory(dev->desc.shell.opaque,
                                              chunk_gpa, chunk_len,
                                              cmd_buf + bytes_read)) {
                LAGFX_WARN("compute drain: ch=%u body chunk DMA failed",
                           chan_id);
                body_ok = false;
                break;
            }
            bytes_read += chunk_len;
        }
        if (!body_ok) break;

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

        dispatch_command(p, &hdr);

        /* Per-channel stamp slot = chan_id (1..4 → SLOT_COMPUTE_1..4). */
        lagfx_protocol_complete_stamp_slot(p, chan_id, stamp);

        p->total_cmds_seen++;
        cmds++;
        rp = rp + length;  /* absolute, monotonic — see per-channel-ring-pfn-array.md */
    }

    /* Publish the drained position back to the descriptor so the
     * kext's watchdog observes progress (channel-progress-flag-RE.md). */
    lagfx_ring_publish_read_ptr(p, desc_gpa, rp);

    LAGFX_LOG("compute drain: ch=%u drained=%zu new rp=0x%x",
              chan_id, cmds, rp);
    return cmds;
}

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
#include "../doorbell.h"
#include "../device.h"
#include "../common/log.h"
#include "protocol/opcodes.h"
#include "protocol/state.h"
#include "handlers/handlers.h"

#include <stddef.h>

/* Little-endian readers (guest protocol is LE). */
static inline uint16_t read_le16(const uint8_t *b) {
    return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}

static inline uint32_t read_le32(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

#define LAGFX_COMPUTE_DRAIN_MAX_CMDS 128u
#define LAGFX_COMPUTE_MAX_CMD_BYTES  4096u

/* FIFORingDescriptor wire layout — 20 bytes (RE:
 * paravirt-re/library/state-machines/FIFORingDescriptor.md). */
#define LAGFX_FIFO_DESC_BYTES        20u
#define LAGFX_FIFO_DESC_BASE_OFFSET  0x400u

/* VirtualChannel ring size per FIFORingDescriptor.md (§"ring size"):
 * 0x10000 (64 KiB).
 *
 * Per per-channel-ring-pfn-array.md: ring_pfn<<12 is NOT command data —
 * it's a u32 PFN-array (page0). Actual command bytes live at
 * (page0[idx]<<12) + (off & 0xfff), where idx = (off % ring_size) >> 12.
 * Descriptor write_ptr/read_ptr are ABSOLUTE monotonic byte counters
 * (NOT ring-modular); take the modulo BEFORE computing page_idx.
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
            lagfx_compute_exec_indirect2(p, hdr);
            break;

        /* Kext-side per-channel exec variant per
         * M4-inner-opcode-implementation-guide.md §1.1 — same outer
         * payload as 0x20, kext emits this on the per-channel exec
         * rings (ch 1..4). Conflicts with LAGFX_OP_CHANNEL_EVENT_37
         * naming in opcodes.h — the kext-disasm pass classified 0x37
         * as "ChannelEventMachine-adjacent" before the M4 RE pass
         * identified it as the per-channel CmdExecIndirect2. The
         * payload's outer layout (12 + dc*24 + rc*16) distinguishes
         * the two at runtime; for now route 0x37 to exec_indirect2
         * which handles both empty (event-only) and populated (real
         * exec) payloads gracefully. */
        case LAGFX_OP_CHANNEL_EVENT_37:  /* 0x37 — see comment above */
            lagfx_compute_exec_indirect2(p, hdr);
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
        case LAGFX_OP_SET_OBJECT_LIST:        /* 0x24 */
            LAGFX_LOG("compute: 0x24 CmdSetObjectList ch=%u stamp=0x%08x payload_size=%u",
                      (unsigned)p->current_chan_id, hdr->stamp,
                      (unsigned)hdr->payload_size);
            break;
        case LAGFX_OP_SET_OBJECT_PLACEMENT:   /* 0x25 */
            LAGFX_LOG("compute: 0x25 CmdSetObjectPlacement ch=%u stamp=0x%08x payload_size=%u",
                      (unsigned)p->current_chan_id, hdr->stamp,
                      (unsigned)hdr->payload_size);
            break;
        case LAGFX_OP_IOSURFACE_LOOKUP:       /* 0x28 */
            LAGFX_LOG("compute: 0x28 CmdIOSurfaceLookup ch=%u stamp=0x%08x payload_size=%u",
                      (unsigned)p->current_chan_id, hdr->stamp,
                      (unsigned)hdr->payload_size);
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

        default:
            LAGFX_WARN("compute dispatch: ch=%u unknown opcode 0x%04x stamp=0x%08x",
                       (unsigned)p->current_chan_id, hdr->opcode, hdr->stamp);
            p->unknown_opcode_count++;
            break;
    }
}

/* Read the 20-byte FIFORingDescriptor for chan_id from the shared
 * control page. Returns true on success. On true, *out_write_ptr,
 * *out_read_ptr, *out_ring_gpa are populated and *out_desc_gpa is the
 * descriptor's GPA (so the caller can publish the bumped read_ptr).
 */
static bool fifo_descriptor_read(lagfx_protocol_t *p,
                                  uint32_t chan_id,
                                  uint64_t *out_desc_gpa,
                                  uint32_t *out_write_ptr,
                                  uint32_t *out_read_ptr,
                                  uint64_t *out_ring_gpa) {
    lagfx_device_t *dev = (lagfx_device_t *)p->dev;
    if (!dev || !dev->desc.shell.read_memory) {
        LAGFX_WARN("fifo_descriptor_read: no shell.read_memory callback");
        return false;
    }
    if (p->ring_shared_page_pfn == 0u) {
        LAGFX_TRACE("fifo_descriptor_read: ring_shared_page_pfn=0 — kext "
                    "hasn't published shared page yet");
        return false;
    }
    if (chan_id == 0u) {
        return false;
    }

    uint64_t shared_gpa = (uint64_t)p->ring_shared_page_pfn << 12;
    uint64_t desc_gpa   = shared_gpa
                        + LAGFX_FIFO_DESC_BASE_OFFSET
                        + (uint64_t)(chan_id - 1u) * LAGFX_FIFO_DESC_BYTES;

    uint8_t desc[LAGFX_FIFO_DESC_BYTES];
    if (!dev->desc.shell.read_memory(dev->desc.shell.opaque,
                                      desc_gpa,
                                      LAGFX_FIFO_DESC_BYTES,
                                      desc)) {
        LAGFX_WARN("fifo_descriptor_read: read failed at 0x%llx (chan=%u)",
                   (unsigned long long)desc_gpa, chan_id);
        return false;
    }

    uint32_t write_ptr = read_le32(desc + 0x00);
    uint32_t read_ptr  = read_le32(desc + 0x04);
    uint32_t desc_chan = read_le32(desc + 0x0c);
    uint32_t ring_pfn  = read_le32(desc + 0x10);

    if (ring_pfn == 0u) {
        LAGFX_TRACE("fifo_descriptor_read: ch=%u ring_pfn=0 — descriptor "
                    "not yet initialised", chan_id);
        return false;
    }
    if (desc_chan != chan_id) {
        LAGFX_WARN("fifo_descriptor_read: ch=%u descriptor chan_id field "
                   "mismatch (got %u) — proceeding with caller's id",
                   chan_id, desc_chan);
    }

    *out_desc_gpa   = desc_gpa;
    *out_write_ptr  = write_ptr;
    *out_read_ptr   = read_ptr;
    /* ring_pfn<<12 is page 0 (PFN-array of u32 data-page PFNs), NOT
     * command data. Caller must resolve via page0[idx]. */
    *out_ring_gpa   = (uint64_t)ring_pfn << 12;
    return true;
}

/* Resolve data-page GPA via page0's PFN-array (see
 * per-channel-ring-pfn-array.md). idx = (off % ring_size) >> 12. */
static bool ring_resolve_data_gpa(lagfx_protocol_t *p,
                                   uint64_t page0_gpa,
                                   uint32_t ring_size,
                                   uint32_t offset,
                                   uint64_t *out_data_gpa) {
    lagfx_device_t *dev = (lagfx_device_t *)p->dev;
    if (!dev || !dev->desc.shell.read_memory) return false;

    uint32_t off_mod = (ring_size != 0u) ? (offset % ring_size) : offset;
    uint32_t page_idx = off_mod >> 12;

    uint8_t pfn_buf[4];
    if (!dev->desc.shell.read_memory(dev->desc.shell.opaque,
                                      page0_gpa + page_idx * 4u,
                                      4, pfn_buf)) {
        LAGFX_WARN("ring_resolve_data_gpa: PFN-array read failed at "
                   "0x%llx", (unsigned long long)(page0_gpa + page_idx * 4u));
        return false;
    }
    uint32_t data_pfn = read_le32(pfn_buf);
    if (data_pfn == 0u) {
        LAGFX_WARN("ring_resolve_data_gpa: PFN-array entry[%u]=0 — ring "
                   "page not mapped at off=0x%x", page_idx, offset);
        return false;
    }
    *out_data_gpa = ((uint64_t)data_pfn << 12) + (off_mod & 0xfffu);
    return true;
}

/* Publish the host's drained position back to the descriptor's read_ptr
 * field (+0x04) — never +0x00 (FIFORingDescriptor.md "Bug pattern"). */
static void fifo_descriptor_publish_rp(lagfx_protocol_t *p,
                                        uint64_t desc_gpa,
                                        uint32_t read_ptr) {
    lagfx_device_t *dev = (lagfx_device_t *)p->dev;
    if (!dev || !dev->desc.shell.write_memory) {
        return;
    }
    if (!dev->desc.shell.write_memory(dev->desc.shell.opaque,
                                       desc_gpa + 0x04u,
                                       sizeof(read_ptr),
                                       &read_ptr)) {
        LAGFX_WARN("fifo_descriptor_publish_rp: write failed at 0x%llx",
                   (unsigned long long)(desc_gpa + 0x04u));
    }
}

/* Drain ring buffer for compute channels. Returns number of commands processed. */
size_t channel_compute_dispatcher_drain(lagfx_protocol_t *p, uint32_t chan_id) {
    if (!lagfx_protocol_is_valid(p)) {
        return 0;
    }

    /* Look up the per-channel ring geometry from the FIFORingDescriptor. */
    uint64_t desc_gpa = 0;
    uint32_t write_ptr = 0;
    uint32_t read_ptr = 0;
    uint64_t page0_gpa = 0;
    if (!fifo_descriptor_read(p, chan_id, &desc_gpa, &write_ptr, &read_ptr, &page0_gpa)) {
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
        if (!ring_resolve_data_gpa(p, page0_gpa, ring_size, rp, &hdr_gpa)) {
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

        uint16_t opcode       = read_le16(hdr_buf + 0);
        uint16_t arg_count_8b = read_le16(hdr_buf + 2);
        uint32_t length       = read_le32(hdr_buf + 4);
        uint32_t stamp        = read_le32(hdr_buf + 8);

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
         * non-contiguous physical pages. */
        uint8_t cmd_buf[LAGFX_COMPUTE_MAX_CMD_BYTES];
        bool body_ok = true;
        uint32_t bytes_read = 0;
        while (bytes_read < length) {
            uint64_t chunk_gpa = 0;
            if (!ring_resolve_data_gpa(p, page0_gpa, ring_size,
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
    fifo_descriptor_publish_rp(p, desc_gpa, rp);

    LAGFX_LOG("compute drain: ch=%u drained=%zu new rp=0x%x",
              chan_id, cmds, rp);
    return cmds;
}

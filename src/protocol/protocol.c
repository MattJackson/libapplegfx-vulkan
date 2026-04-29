/*
 * libapplegfx-vulkan — protocol decoder lifecycle + dispatcher
 * src/protocol/protocol.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Decoder lifecycle, MMIO register shadow + per-channel doorbell
 * (BAR0+0x1020), and the per-cmd dispatch + stamp-completion path.
 * Tests drive dispatch via lagfx_protocol_dispatch_one directly.
 */

#include "protocol.h"
#include "state.h"
#include "opcodes.h"
#include "fifo.h"
#include "ops_display_vchan.h"
#include "../device.h"
#include "../common/log.h"

#include <stdlib.h>
#include <string.h>

/* === Lifecycle ============================================== */

lagfx_protocol_t *lagfx_protocol_new(struct lagfx_device *dev) {
    if (!lagfx_device_is_valid(dev)) {
        LAGFX_ERR("protocol_new: invalid device %p", (void *)dev);
        return NULL;
    }

    lagfx_protocol_t *p =
        (lagfx_protocol_t *)calloc(1, sizeof(*p));
    if (!p) {
        LAGFX_ERR("protocol_new: out of memory");
        return NULL;
    }

    p->magic = LAGFX_PROTOCOL_MAGIC;
    p->dev   = dev;

    /* Register defaults. Bit 0 of STATUS_CONTROL = "present", bit 1
     * = "ready" per §3.1 of the brief (inferred). */
    p->reg[0] = 0x3u;  /* STATUS_CONTROL */

    LAGFX_LOG("protocol_new: p=%p dev=%p", (void *)p, (void *)dev);
    return p;
}

void lagfx_protocol_free(lagfx_protocol_t *p) {
    if (!p) {
        return;
    }
    if (p->magic != LAGFX_PROTOCOL_MAGIC) {
        LAGFX_ERR("protocol_free: bad magic on %p (got 0x%08x)",
                  (void *)p, p->magic);
        return;
    }
    LAGFX_LOG("protocol_free: p=%p (seen=%llu, completed=%llu, unknown=%llu)",
              (void *)p,
              (unsigned long long)p->total_cmds_seen,
              (unsigned long long)p->total_cmds_completed,
              (unsigned long long)p->unknown_opcode_count);
    memset(p, 0, sizeof(*p));
    free(p);
}

void lagfx_protocol_reset(lagfx_protocol_t *p) {
    if (!lagfx_protocol_is_valid(p)) {
        return;
    }

    LAGFX_LOG("protocol_reset: p=%p", (void *)p);

    /* Clear tables and inflight; preserve ring geometry and registers. */
    memset(p->tasks,    0, sizeof(p->tasks));
    memset(p->fifos,    0, sizeof(p->fifos));
    memset(p->inflight, 0, sizeof(p->inflight));
    memset(p->displays, 0, sizeof(p->displays));
    memset(&p->resources, 0, sizeof(p->resources));

    p->total_cmds_seen      = 0;
    p->total_cmds_completed = 0;
    p->unknown_opcode_count = 0;
    p->interrupts_raised    = 0;
    p->last_completed_stamp = 0;
    p->read_ptr             = 0;
    p->write_ptr            = 0;
    p->pending_stamps_bitmask = 0;

    p->display_swaps_applied          = 0;
    p->display_transactions_submitted = 0;
    p->display_acks_received          = 0;
}

/* === Completion path ========================================
 *
 * Every command unconditionally signals its stamp when done. Per
 * A4d (2026-04-24): the kext's unified ISR on MSI-X vec 0 reads
 * BAR0+0x1018 as a stamp-completion bitmask and feeds it to
 * AppleParavirtEventMachine::signalStamps. Per-completion work:
 *   1. monotonically advance *stampBases[slot] in DMA-visible memory
 *      (see library/howto/how-to-host-stamp-completion.md),
 *   2. OR bit `slot` into pending_stamps_bitmask shadow,
 *   3. raise MSI vec 0.
 * The ISR loops set bits, calls commandWakeup(slot) per stamp, which
 * reads the cell via stampBases[slot] = u32 @ FIFO+slot*4. */

/* Monotonic stamp-cell advance — never regresses. Reads current cell,
 * writes max(target, cur+1), with a 1 floor (never 0). All callers
 * that update a stamp cell MUST go through this helper, otherwise a
 * stale write < current value will park the kext until the bounded
 * 1-second deadline kicks in (waitForStamp-deadline-semantics.md). */
static void lagfx_advance_stamp_cell(lagfx_protocol_t *p,
                                     uint32_t slot,
                                     uint32_t target_stamp) {
    if (!p || !p->dev || !p->dev->desc.shell.write_memory
        || p->ring_base_pfn == 0u) {
        return;
    }
    uint64_t cell_gpa = ((uint64_t)p->ring_base_pfn << 12)
                        + (uint64_t)slot * 4u;
    uint32_t cur = 0u;
    if (p->dev->desc.shell.read_memory) {
        p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                       cell_gpa, sizeof(cur), &cur);
    }
    /* max(target, cur+1) with a non-zero floor. */
    uint32_t want = (target_stamp > cur + 1u) ? target_stamp : (cur + 1u);
    if (want == 0u) {
        want = 1u;
    }
    if (p->dev->desc.shell.write_memory(
            p->dev->desc.shell.opaque,
            cell_gpa, sizeof(want), &want)) {
                LAGFX_LOG("stamp_cell[%u] := %u (was %u, target=%u, gpa=0x%llx)",
                  slot, want, cur, target_stamp,
                  (unsigned long long)cell_gpa);
    }
}

void lagfx_protocol_complete_stamp_slot(lagfx_protocol_t *p,
                                        uint32_t slot,
                                        uint32_t stamp) {
    if (!lagfx_protocol_is_valid(p)) {
        return;
    }

    p->last_completed_stamp = stamp;
    p->total_cmds_completed += 1;

    lagfx_advance_stamp_cell(p, slot, stamp);

    p->pending_stamps_bitmask |= (1u << slot);

    if (p->dev && p->dev->desc.shell.raise_interrupt) {
        /* Per A4d (2026-04-24): the accelerator kext registers exactly
         * one interrupt source at MSI-X vector 0. A single unified ISR
         * demuxes stamps / displays / faults off BAR0 status regs. No
         * separate "stamp-interrupt" vector exists; vec 1 hits another
         * kext's ISR and breaks guest networking. Pin to vec 0. */
        p->dev->desc.shell.raise_interrupt(p->dev->desc.shell.opaque, 0u);
        p->interrupts_raised += 1;
        LAGFX_TRACE("complete_stamp[slot=%u]: cmd_stamp=0x%08x + IRQ vec=0 "
                    "(pending_mask=0x%08x)",
                    slot, stamp, p->pending_stamps_bitmask);
    } else {
        LAGFX_TRACE("complete_stamp[slot=%u]: cmd_stamp=0x%08x (no IRQ cb)",
                    slot, stamp);
    }
}

void lagfx_protocol_complete_stamp(lagfx_protocol_t *p, uint32_t stamp) {
    /* RootChannel completions land on slot 0. Confirmed A6e: every
     * init-phase dispatcher-driven command emits `xor esi, esi` before
     * writeStamp. Doorbell-driven completions for per-channel rings
     * (ch >= 1) call lagfx_protocol_complete_stamp_slot directly with
     * the per-channel slot. */
    lagfx_protocol_complete_stamp_slot(p, 0u, stamp);
}

/* === Dispatcher ============================================ */

/* Internal: parse + run the handler. Caller decides what (if any)
 * stamp completion to run on the way out. Returns rc; on parse
 * failure returns negative value and *did_run_handler = 0. */
static int lagfx_dispatch_inner(lagfx_protocol_t *p,
                                const uint8_t *cmd_bytes, size_t cmd_len,
                                lagfx_cmd_header_t *out_hdr,
                                int *did_run_handler) {
    *did_run_handler = 0;
    if (!lagfx_protocol_is_valid(p) || !cmd_bytes) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    if (!lagfx_fifo_parse_header(cmd_bytes, cmd_len, out_hdr)) {
        LAGFX_ERR("dispatch: header parse failed (len=%zu)", cmd_len);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    p->total_cmds_seen += 1;

    const lagfx_op_descriptor_t *desc = lagfx_opcode_lookup(out_hdr->opcode);
    lagfx_op_handler_fn fn = (desc && desc->handler) ? desc->handler
                                                     : lagfx_op_default_handler;

    if (!desc) {
        p->unknown_opcode_count += 1;
    }

    LAGFX_TRACE("dispatch: op=0x%04x (%s) stamp=0x%08x arg_count_8b=%u "
                "length=%u payload=%u",
                out_hdr->opcode, lagfx_opcode_name(out_hdr->opcode),
                out_hdr->stamp, (unsigned)out_hdr->arg_count_8b,
                (unsigned)out_hdr->length, (unsigned)out_hdr->payload_size);

    if (desc) {
        if (out_hdr->payload_size < desc->min_payload) {
            LAGFX_WARN("dispatch: %s payload too small (%u < %u min)",
                       desc->name, (unsigned)out_hdr->payload_size,
                       (unsigned)desc->min_payload);
            return LAGFX_HANDLER_ERR_SIZE;
        }
        if (desc->max_payload != 0 && out_hdr->payload_size > desc->max_payload) {
            LAGFX_WARN("dispatch: %s payload too large (%u > %u max)",
                       desc->name, (unsigned)out_hdr->payload_size,
                       (unsigned)desc->max_payload);
            /* Fall through — fail-open. */
        }
    }

    *did_run_handler = 1;
    return (int)fn(p, out_hdr);
}

int lagfx_protocol_dispatch_one(lagfx_protocol_t *p,
                                const uint8_t *cmd_bytes,
                                size_t cmd_len) {
    lagfx_cmd_header_t hdr;
    int did_run = 0;
    int rc = lagfx_dispatch_inner(p, cmd_bytes, cmd_len, &hdr, &did_run);
    /* RootChannel completions go to slot 0; whether the handler ran
     * or we hit a parse/size error, we ack the stamp so the guest
     * doesn't park. */
    if (did_run || rc == LAGFX_HANDLER_ERR_SIZE) {
        lagfx_protocol_complete_stamp(p, hdr.stamp);
    }
    return rc;
}

/* lagfx_task_translate moved to translate.c. */

/* Per-channel variant — runs the handler but does NOT auto-complete
 * the stamp. The caller (typically the per-channel doorbell handler)
 * is responsible for advancing stamp_cell[ch] + setting the
 * pending_stamps_bitmask bit + raising the IRQ once after draining
 * all cmds in the ring. Returns the handler rc and writes the
 * parsed header to *out_hdr if non-NULL. */
int lagfx_protocol_dispatch_one_no_stamp(lagfx_protocol_t *p,
                                         const uint8_t *cmd_bytes,
                                         size_t cmd_len,
                                         lagfx_cmd_header_t *out_hdr) {
    lagfx_cmd_header_t local_hdr;
    int did_run = 0;
    int rc = lagfx_dispatch_inner(p, cmd_bytes, cmd_len, &local_hdr, &did_run);
    (void)did_run;
    if (out_hdr) {
        *out_hdr = local_hdr;
    }
    return rc;
}

/* === MMIO register shadow =================================== */

uint32_t lagfx_protocol_mmio_read(lagfx_protocol_t *p, uint64_t offset) {
    if (!lagfx_protocol_is_valid(p)) {
        return 0;
    }

    /* MSI-X table range — shell owns it. Return 0, no log spam. */
    if (offset < LAGFX_MSIX_RANGE_END) {
        return 0;
    }

    /*
     * Secondary capability table (BAR0+0x1200..0x122c).
     *
     * AppleParavirtGPUControl::start()+0x7c reads a u32 at BAR0+0x122c
     * and branches on its value:
     *   0       -> "no-caps" bail (silent no-op; M2 invisible)
     *   1..8    -> legacy / pre-v9 shape
     *   >=9     -> modern capability path (what we want)
     * Disasm: paravirt-re/baselines/phase-1d4-disasm.txt:42-48.
     *
     * Return 9 to select the modern path. Rest of the 0x1200..0x1228
     * block returns 0 (no optional features advertised); those aren't
     * consumed by start() before +0x7c, so safe default for now.
     */
    if (offset == 0x122c) {
        return 9;
    }
    if (offset >= 0x1200 && offset < 0x122c) {
        return 0;
    }

    /* 0x100c is the guest-observable read pointer; return a live
     * value so the kext can poll our drain progress. */
    if (offset == LAGFX_REG_FIFO_FAULT_OFFSET) {
        LAGFX_TRACE("mmio_read: FIFO_FAULT_OFFSET -> 0x%x (read_ptr)",
                  p->read_ptr);
        return p->read_ptr;
    }

    /*
     * 0x102c — fault-pending status.
     *
     * A4d (2026-04-24): the kext's unified ISR at MSI-X vec 0 reads
     * this register to decide whether to walk the fault queue via
     * handleFaultInterrupt. Non-zero = faults pending. Returning
     * `last_completed_stamp` (our previous behavior) caused every ISR
     * to spuriously drain the fault queue, which in turn would IOLog
     * each entry and eventually escalate to terminate. Return 0
     * unconditionally unless we actually have a fault to report;
     * no fault path is wired yet.
     */
    if (offset == 0x102cu) {
        LAGFX_TRACE("mmio_read: 0x102c (fault_status) -> 0");
        return 0u;
    }

    /*
     * 0x1018 — stamp-completion bitmask fed to signalStamps.
     *
     * A4d (2026-04-24): bit N = stamp_id N completed since the last
     * ISR read. Kext does xchg-with-0 on this register and passes the
     * prior value to AppleParavirtEventMachine::signalStamps, which
     * iterates set bits via bsf and calls commandWakeup(stamp_id) per
     * bit. commandWakeup reads the actual stamp value from [EM+0x20]
     * in kernel heap — we don't have to provide stamp values via DMA.
     *
     * Return our pending-stamps bitmask and atomically clear it.
     */
    if (offset == LAGFX_REG_STAMP_CELL_2) {
        uint32_t mask = p->pending_stamps_bitmask;
        p->pending_stamps_bitmask = 0u;
        LAGFX_TRACE("mmio_read: 0x1018 (stamp_bitmask) xchg -> 0x%x", mask);
        return mask;
    }

    /*
     * 0x1014 — display-interrupt bitmask fed to signalDisplays.
     *
     * A4d (2026-04-24): same pattern as 0x1018, but the mask here
     * targets AppleParavirtDisplayMachine. Bit N = display_id N has
     * a completed transaction. No displays complete during the M3
     * init path, so returning 0 is the correct behaviour until we
     * wire display-transaction completions through a second bitmask.
     */
    if (offset == LAGFX_REG_STAMP_CELL_1) {
        /* xchg-and-clear, mirrors 0x1018 semantics (per A4d).
         * Bit N = display channel N has a completed transaction. Set by
         * the per-channel doorbell handler for display channels (ch>=5).
         * Per display0-cmd-actual-location.md, the display bitmask is
         * what signalDisplays consumes — separate from the stamp bitmask. */
        uint32_t mask = p->pending_displays_bitmask;
        p->pending_displays_bitmask = 0u;
        LAGFX_TRACE("mmio_read: 0x1014 (display_bitmask) xchg -> 0x%x", mask);
        return mask;
    }

    int idx = lagfx_protocol_reg_index(offset);
    if (idx < 0) {
        LAGFX_TRACE("mmio_read: unmapped offset 0x%llx -> 0",
                  (unsigned long long)offset);
        return 0;
    }

    uint32_t value = p->reg[idx];

    LAGFX_TRACE("mmio_read: off=0x%llx -> 0x%08x",
              (unsigned long long)offset, value);
    return value;
}

void lagfx_protocol_mmio_write(lagfx_protocol_t *p, uint64_t offset,
                               uint32_t value) {
    if (!lagfx_protocol_is_valid(p)) {
        return;
    }

    /* MSI-X range — shell's problem. */
    if (offset < LAGFX_MSIX_RANGE_END) {
        return;
    }

    int idx = lagfx_protocol_reg_index(offset);
    if (idx < 0) {
        LAGFX_TRACE("mmio_write: unmapped offset 0x%llx val=0x%08x",
                  (unsigned long long)offset, value);
        return;
    }

    /* Shadow first so reads reflect the write even if we short-circuit
     * into a setter probe. Note that 0x101c is documented read-only in
     * the dylib (returns _rootPageNumber); we still shadow the written
     * value so a subsequent read in the test rig can confirm the path
     * was taken. The real hardware behavior on write to 0x101c is also
     * "store to setter-backed ivar" (_PGDevice calls its 1-arg
     * selector), so shadowing is consistent. */
    p->reg[idx] = value;

    LAGFX_TRACE("mmio_write: off=0x%llx val=0x%08x",
              (unsigned long long)offset, value);

    /* Primary-ring MMIO write map (resolved 2026-04-21 from live M2
     * MMIO trace + Agent I disasm of Accelerator::setupCommandRing).
     *
     *   0x1000 W → ring_armed (1=enable). Kick only; doorbell advances
     *              happen via 0x1008.
     *   0x1004 W → ring_size (bytes; observed 0x10000 = 64 KiB).
     *   0x1008 W → write_ptr update. Each write advances write_ptr;
     *              we drain everything in [read_ptr, write_ptr).
     *   0x100c R → read_ptr (guest polls to confirm our progress).
     *              Read handler is elsewhere; we don't write 0x100c.
     *   0x1010 W → page_size (observed 0x1000).
     *   0x101c W → ring_shared_page_pfn (mailbox page; NOT the
     *              command ring). Previously mistagged read-only.
     *   0x1030 W → ring_base_pfn → ring_base_gpa = pfn << 12.
     */
    switch (offset) {
        case LAGFX_REG_STATUS_CONTROL:
            p->ring_armed = (value != 0u);
            LAGFX_TRACE("mmio_write: STATUS_CONTROL ring_armed=%d",
                      (int)p->ring_armed);
            return;
        case 0x1004u:
            p->ring_size = value ? value : 0x10000u;
            LAGFX_TRACE("mmio_write: ring_size=0x%x", p->ring_size);
            return;
        case 0x1008u: {
            uint32_t old_wp = p->write_ptr;
            p->write_ptr = value;
            LAGFX_TRACE("mmio_write: doorbell wp=0x%x (was 0x%x)",
                      value, old_wp);
            if (p->ring_armed && p->write_ptr != p->read_ptr) {
                size_t drained = lagfx_fifo_drain(p);
                LAGFX_TRACE("mmio_write: doorbell drained %zu cmds", drained);
            }
            return;
        }
        case 0x1010u:
            /* setFifoStart — byte offset of ring within its base page.
             * Observed value 0x1000 on M2 boot: first 4 KiB of the
             * base page is a control/metadata header; command stream
             * starts at base_page + 0x1000. */
            p->ring_start_offset = value;
            p->page_size = 0x1000u;  /* assumed; not read from MMIO */
            p->ring_base_gpa =
                ((uint64_t)p->ring_base_pfn << 12) + p->ring_start_offset;
            LAGFX_TRACE("mmio_write: ring_start_offset=0x%x -> gpa=0x%llx",
                      value, (unsigned long long)p->ring_base_gpa);
            return;
        case 0x101cu:
            p->ring_shared_page_pfn = value;
            LAGFX_TRACE("mmio_write: ring_shared_page_pfn=0x%x", value);
            return;
        case 0x1030u: {
            p->ring_base_pfn = value;
            p->ring_base_gpa =
                ((uint64_t)value << 12) + p->ring_start_offset;
            if (p->ring_size == 0u) {
                p->ring_size = 0x10000u;
            }
            LAGFX_TRACE("mmio_write: ring_base_pfn=0x%x -> gpa=0x%llx size=0x%x",
                      value, (unsigned long long)p->ring_base_gpa,
                      p->ring_size);
            return;
        }
        case 0x1020u: {
            /* Per-channel doorbell. Kext writes ch_id=N to signal that it has
             * placed work on child-channel N's ring. Per re-followup-spec-gaps
             * §13.2.3, the kext pre-allocates ring geometry and writes a
             * 20-byte descriptor at:
             *   shared_pfn<<12 + 0x400 + 20*(idx-1)
             * containing: { ring_pfn, read_ptr, write_ptr, len, flags }.
             *
             * This handler reads that descriptor, drains commands from
             * read_ptr to write_ptr (logging unknown opcodes), advances the
             * read_ptr atomically in the descriptor, and signals the channel
             * via global stamp bitmask + IRQ. Per IOAccelFIFOChannel2-restart-RE
             * memo, advancing read_ptr is the operation the GPU-hang watchdog
             * is polling for at "Display0 written: 20 read: 0 cmd: 01...".
             *
             * Display0 = ch_id 5 (per A10a: display_index+5). */
            unsigned ch = value;
            if (ch == 0u || ch >= 32u) {
                LAGFX_TRACE("doorbell ch=%u: out of range", ch);
                return;
            }
            if (p->ring_shared_page_pfn == 0u
                || !p->dev || !p->dev->desc.shell.read_memory) {
                LAGFX_TRACE("doorbell ch=%u: shared page unknown or no read cb",
                          ch);
                return;
            }

            uint64_t shared_gpa = (uint64_t)p->ring_shared_page_pfn << 12;
            uint64_t descr_gpa = shared_gpa + 0x400u + 20u * (ch - 1u);
            uint8_t descr[20] = {0};
            if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                                  descr_gpa,
                                                  sizeof(descr), descr)) {
                LAGFX_TRACE("doorbell ch=%u: read_memory failed at gpa=0x%llx",
                          ch, (unsigned long long)descr_gpa);
                return;
            }

            /* Field order verified via QEMU monitor xp dump 2026-04-25 of
             * Display0 (idx=5) descriptor at shared+0x450:
             *   { write_ptr=0x14, read_ptr=0, ?=0, chan_id=5, ring_pfn=0x3d61a8 }
             * Differs from agent's RE memo (which guessed
             * {ring_pfn, read_ptr, write_ptr, len, flags}). The "GPU hang:
             * Name DisplayN written: %u read: %u" log values match exactly:
             * write_ptr is u32[0], read_ptr is u32[1]. */
            uint32_t write_ptr = ((uint32_t *)descr)[0];
            uint32_t read_ptr  = ((uint32_t *)descr)[1];
            uint32_t mid       = ((uint32_t *)descr)[2];
            uint32_t chan_id   = ((uint32_t *)descr)[3];
            uint32_t ring_pfn  = ((uint32_t *)descr)[4];

            LAGFX_TRACE("doorbell ch=%u: descr wr=%u rd=%u mid=0x%x "
                      "chan_id=%u ring_pfn=0x%x",
                      ch, write_ptr, read_ptr, mid, chan_id, ring_pfn);

            if (chan_id != ch) {
                LAGFX_WARN("doorbell ch=%u: chan_id mismatch "
                          "(descr says %u), aborting", ch, chan_id);
                return;
            }

            if (ch >= 1u && ch <= 4u
                && (ring_pfn == 0u || write_ptr <= read_ptr
                    || write_ptr > 0x100000u)) {
                LAGFX_WARN("doorbell ch=%u: guard FAIL ring_pfn=0x%x "
                          "wp=%u rp=%u — skipping drain",
                          ch, ring_pfn, write_ptr, read_ptr);
                return;
            }

            /* Per-channel ring drain for non-display channels (ch 1..4).
             * Carries actual GPU work (CmdExecIndirect2 with full payload,
             * CmdSyncResources, CmdMapMemory2, etc.) — must dispatch each
             * cmd through the opcode handlers, NOT treat as setupSharedState.
             * Display channels (ch 5..7) carry only setupSharedState; that
             * code path is below.
             *
             * Wire format (CONFIRMED 2026-04-26 trace):
             *   ring_data_gpa starts at page0[0]<<12 (PFN-array indirection).
             *   First cmd at ring_data_gpa + read_ptr; advance by hdr.length
             *   per cmd until cur_rp == write_ptr.
             *
             * For now: log + dispatch_one_no_stamp each cmd. After the walk,
             * advance descr.read_ptr to write_ptr, advance stamp_cell[ch]
             * via the monotonic helper using the LAST cmd's stamp value,
             * set bitmask bit ch, raise IRQ once. */
            if (ch >= 1u && ch <= 4u
                && ring_pfn != 0u && write_ptr > read_ptr
                && write_ptr <= 0x100000u) {
                uint64_t ring_gpa_base = ((uint64_t)ring_pfn << 12);
                uint32_t child_ring_size = 0x10000u;

                /* page0 is a u32 PFN-array; data_pfn[i] is at page0+i*4.
                 * Cmd at ring offset `off` lives in data_pfn[off/0x1000]
                 * at page-offset (off & 0xfff). Cmds CAN cross page
                 * boundaries — read in per-page chunks and stitch.
                 *
                 * Diagnostic: on first drain for each channel, dump
                 * the first 16 u32 entries of page0 to understand the
                 * PFN-array layout when it fails. */
                {
                    static uint32_t ch_drained[32] = {0};
                    if (ch < 32u && ch_drained[ch] == 0u) {
                        ch_drained[ch] = 1u;
                        uint32_t pfn_dump[16] = {0};
                        p->dev->desc.shell.read_memory(
                            p->dev->desc.shell.opaque,
                            ring_gpa_base, sizeof(pfn_dump), pfn_dump);
                        LAGFX_WARN("doorbell ch=%u FIRST DRAIN: "
                                   "ring_pfn=0x%x ring_gpa_base=0x%llx "
                                   "rp=%u wp=%u "
                                   "page0[0..15]=%08x %08x %08x %08x "
                                   "%08x %08x %08x %08x "
                                   "%08x %08x %08x %08x "
                                   "%08x %08x %08x %08x",
                                   ch, ring_pfn,
                                   (unsigned long long)ring_gpa_base,
                                   read_ptr, write_ptr,
                                   pfn_dump[0], pfn_dump[1],
                                   pfn_dump[2], pfn_dump[3],
                                   pfn_dump[4], pfn_dump[5],
                                   pfn_dump[6], pfn_dump[7],
                                   pfn_dump[8], pfn_dump[9],
                                   pfn_dump[10], pfn_dump[11],
                                   pfn_dump[12], pfn_dump[13],
                                   pfn_dump[14], pfn_dump[15]);
                    }
                }

                uint32_t last_stamp = 0u;
                uint32_t cur_rp = read_ptr;
                unsigned cmd_idx = 0;
                LAGFX_LOG("doorbell ch=%u drain start: read_ptr=%u write_ptr=%u",
                          ch, read_ptr, write_ptr);
                while (cur_rp + 12u <= write_ptr) {
                    uint8_t hdr_bytes[12];
                    {
                        bool ok = true;
                        uint32_t off = cur_rp;
                        size_t got = 0;
                        while (got < 12u && ok) {
                            uint32_t mod_off = off % child_ring_size;
                            uint32_t page_idx = mod_off >> 12;
                            uint32_t page_off = mod_off & 0xfffu;
                            uint32_t can = 0x1000u - page_off;
                            uint32_t want = (uint32_t)(12u - got);
                            uint32_t take = (want < can) ? want : can;
                            uint32_t pte_pfn = 0u;
                            if (!p->dev->desc.shell.read_memory(
                                    p->dev->desc.shell.opaque,
                                    ring_gpa_base + (uint64_t)page_idx * 4u,
                                    sizeof(pte_pfn), &pte_pfn)) {
                                LAGFX_WARN("doorbell ch=%u: PFN-array read "
                                           "failed at page_idx=%u "
                                           "(gpa=0x%llx)",
                                           ch, page_idx,
                                           (unsigned long long)(
                                               ring_gpa_base +
                                               (uint64_t)page_idx * 4u));
                                ok = false; break;
                            }
                            if (pte_pfn == 0u) {
                                LAGFX_WARN("doorbell ch=%u: PFN-array "
                                           "entry[%u]=0 (unmapped) at "
                                           "rp=%u off=%u page_idx=%u",
                                           ch, page_idx, cur_rp, off,
                                           page_idx);
                                ok = false; break;
                            }
                            if (!p->dev->desc.shell.read_memory(
                                    p->dev->desc.shell.opaque,
                                    ((uint64_t)pte_pfn << 12) + page_off,
                                    take, hdr_bytes + got)) {
                                ok = false; break;
                            }
                            off += take;
                            got += take;
                        }
                        if (!ok) {
                            LAGFX_WARN("doorbell ch=%u: read_memory failed "
                                       "for cmd header at rp=%u",
                                       ch, cur_rp);
                            break;
                        }
                    }
                    uint32_t cmd_len = (uint32_t)(hdr_bytes[4]
                                                  | (hdr_bytes[5] << 8)
                                                  | (hdr_bytes[6] << 16)
                                                  | (hdr_bytes[7] << 24));
                    /* Reject patently absurd cmd_len values BEFORE arithmetic
                     * to avoid uint32 overflow in `cur_rp + cmd_len > wp`. */
                    if (cmd_len < 12u || cmd_len > (write_ptr - cur_rp)) {
                        LAGFX_WARN("doorbell ch=%u: bad cmd_len=%u at rp=%u "
                                   "(wp=%u, available=%u) — stopping walk",
                                   ch, cmd_len, cur_rp, write_ptr,
                                   write_ptr - cur_rp);
                        break;
                    }

                    /* Read the full cmd into a temporary buffer + dispatch.
                     * Same per-page walk as the header read above so cmds
                     * crossing page boundaries assemble correctly. */
                    uint8_t *cmd = malloc(cmd_len);
                    if (!cmd) {
                        LAGFX_WARN("doorbell ch=%u: malloc(%u) failed",
                                   ch, cmd_len);
                        break;
                    }
                    {
                        bool ok = true;
                        uint32_t off = cur_rp;
                        size_t got = 0;
                        while (got < cmd_len && ok) {
                            uint32_t mod_off = off % child_ring_size;
                            uint32_t page_idx = mod_off >> 12;
                            uint32_t page_off = mod_off & 0xfffu;
                            uint32_t can = 0x1000u - page_off;
                            uint32_t want = (uint32_t)(cmd_len - got);
                            uint32_t take = (want < can) ? want : can;
                            uint32_t pte_pfn = 0u;
                            if (!p->dev->desc.shell.read_memory(
                                    p->dev->desc.shell.opaque,
                                    ring_gpa_base + (uint64_t)page_idx * 4u,
                                    sizeof(pte_pfn), &pte_pfn) || pte_pfn == 0u) {
                                ok = false; break;
                            }
                            if (!p->dev->desc.shell.read_memory(
                                    p->dev->desc.shell.opaque,
                                    ((uint64_t)pte_pfn << 12) + page_off,
                                    take, cmd + got)) {
                                ok = false; break;
                            }
                            off += take;
                            got += take;
                        }
                        if (!ok) {
                            LAGFX_WARN("doorbell ch=%u: read_memory(cmd, %u) "
                                       "failed at rp=%u (paginated walk)",
                                       ch, cmd_len, cur_rp);
                            free(cmd);
                            break;
                        }
                    }

                    lagfx_cmd_header_t parsed;
                    p->extra_stamp_advance = 0u;
                    int rc = lagfx_protocol_dispatch_one_no_stamp(
                        p, cmd, cmd_len, &parsed);
                    LAGFX_LOG("doorbell ch=%u cmd[%u]: opcode=0x%04x "
                              "stamp=0x%08x len=%u rc=%d "
                              "extra_stamp_advance=%u "
                              "hdr_bytes=%02x %02x %02x %02x %02x %02x "
                              "%02x %02x %02x %02x %02x %02x",
                              ch, cmd_idx, parsed.opcode, parsed.stamp,
                              cmd_len, rc,
                              p->extra_stamp_advance,
                              cmd[0], cmd[1], cmd[2], cmd[3],
                              cmd[4], cmd[5], cmd[6], cmd[7],
                              cmd[8], cmd[9], cmd[10], cmd[11]);

                    uint32_t effective = parsed.stamp
                                         + p->extra_stamp_advance;
                    LAGFX_LOG("doorbell ch=%u cmd[%u]: effective=0x%08x last_stamp=0x%08x",
                              ch, cmd_idx, effective, last_stamp);
                    if (effective > last_stamp) {
                        last_stamp = effective;
                    }
                    free(cmd);

                    cur_rp += cmd_len;
                    cmd_idx += 1;
                }

                LAGFX_LOG("doorbell ch=%u drain done: %u cmd(s), final last_stamp=0x%08x",
                          ch, cmd_idx, last_stamp);

                uint32_t new_rp = cur_rp;
                if (p->dev->desc.shell.write_memory) {
                    p->dev->desc.shell.write_memory(
                        p->dev->desc.shell.opaque,
                        descr_gpa + 4u, sizeof(new_rp), &new_rp);
                }
                /* Advance stamp_cell[ch] (slot=ch). */
                lagfx_advance_stamp_cell(p, ch, last_stamp);
                p->pending_stamps_bitmask |= (1u << ch);
                if (p->dev->desc.shell.raise_interrupt) {
                    p->dev->desc.shell.raise_interrupt(
                        p->dev->desc.shell.opaque, 0u);
                    p->interrupts_raised += 1;
                }
                LAGFX_TRACE("doorbell ch=%u: drained %u cmd(s), rp=%u->%u, "
                          "stamp_cell+IRQ (pending_mask=0x%08x)",
                          ch, cmd_idx, read_ptr, new_rp,
                          p->pending_stamps_bitmask);
                return;
            }

            /* Display channel opcode dispatch loop (ch >= 5).
             * The display vchan uses a separate opcode namespace:
             *   0x01 = setupSharedState  (8 bytes payload)
             *   0x06 = present           (12 bytes payload)
             *   0x07 = present+gamma     (36 bytes payload)
             * Resolve data pages via page0[0] PFN-array indirection,
             * then walk the ring dispatching each command. */
            uint32_t last_stamp = 0u;
            if (ring_pfn != 0u && write_ptr > read_ptr
                && write_ptr <= 0x100000u) {
                uint64_t ring_gpa_base = ((uint64_t)ring_pfn << 12);
                uint32_t data_pfn = 0u;
                p->dev->desc.shell.read_memory(
                    p->dev->desc.shell.opaque,
                    ring_gpa_base, sizeof(data_pfn), &data_pfn);
                uint64_t ring_data_gpa;
                if (data_pfn != 0u) {
                    ring_data_gpa = (uint64_t)data_pfn << 12;
                } else {
                    ring_data_gpa = ring_gpa_base + 0x1000u;
                }
                LAGFX_TRACE("doorbell ch=%u: data via page0 PFN=0x%x "
                          "(data_gpa=0x%llx)",
                          ch, data_pfn,
                          (unsigned long long)ring_data_gpa);

                uint32_t cur_rp = read_ptr;
                unsigned cmd_idx = 0;
                while (cur_rp + 12u <= write_ptr) {
                    uint8_t hdr_bytes[12] = {0};
                    if (!p->dev->desc.shell.read_memory(
                            p->dev->desc.shell.opaque,
                            ring_data_gpa + cur_rp, 12,
                            hdr_bytes)) {
                        LAGFX_WARN("doorbell ch=%u: read_memory failed "
                                   "for cmd header at rp=%u",
                                   ch, cur_rp);
                        break;
                    }

                    uint16_t opcode = (uint16_t)(hdr_bytes[0]
                                                  | (hdr_bytes[1] << 8));
                    uint32_t cmd_len =
                        (uint32_t)(hdr_bytes[4] | (hdr_bytes[5] << 8)
                                   | (hdr_bytes[6] << 16)
                                   | (hdr_bytes[7] << 24));
                    uint32_t stamp =
                        (uint32_t)(hdr_bytes[8] | (hdr_bytes[9] << 8)
                                   | (hdr_bytes[10] << 16)
                                   | (hdr_bytes[11] << 24));

                    if (cmd_len < 12u
                        || cmd_len > (write_ptr - cur_rp)) {
                        LAGFX_WARN("doorbell ch=%u: bad cmd_len=%u "
                                   "at rp=%u (wp=%u) — stopping walk",
                                   ch, cmd_len, cur_rp, write_ptr);
                        break;
                    }

                    uint32_t payload_len = cmd_len - 12u;
                    uint8_t *payload_buf = NULL;
                    if (payload_len > 0u) {
                        payload_buf = (uint8_t *)malloc(payload_len);
                        if (!payload_buf) {
                            LAGFX_WARN("doorbell ch=%u: malloc(%u) "
                                       "failed", ch, payload_len);
                            break;
                        }
                        if (!p->dev->desc.shell.read_memory(
                                p->dev->desc.shell.opaque,
                                ring_data_gpa + cur_rp + 12u,
                                payload_len, payload_buf)) {
                            LAGFX_WARN("doorbell ch=%u: read_memory "
                                       "failed for payload at rp=%u",
                                       ch, cur_rp);
                            free(payload_buf);
                            break;
                        }
                    }

                    lagfx_cmd_header_t parsed;
                    parsed.opcode       = opcode;
                    parsed.arg_count_8b = 0u;
                    parsed.length       = cmd_len;
                    parsed.stamp        = stamp;
                    parsed.payload_size = (uint16_t)payload_len;
                    parsed.payload      = payload_buf;

                    LAGFX_TRACE("doorbell ch=%u vchan cmd[%u]: "
                              "opcode=0x%02x stamp=0x%08x len=%u",
                              ch, cmd_idx, opcode, stamp, cmd_len);

                    switch (opcode) {
                        case 0x01u:
                            lagfx_op_vchan_setup_shared_state(
                                p, &parsed);
                            break;
                        case 0x02u:
                            lagfx_op_vchan_display_submit(
                                p, &parsed);
                            break;
                        case 0x06u:
                            lagfx_op_vchan_present(p, &parsed);
                            break;
                        case 0x07u:
                            lagfx_op_vchan_present_gamma(p, &parsed);
                            break;
                        default:
                            LAGFX_WARN("doorbell ch=%u: unknown "
                                       "display vchan opcode 0x%02x "
                                       "at rp=%u len=%u",
                                       ch, opcode, cur_rp, cmd_len);
                            break;
                    }

                    if (stamp > last_stamp) {
                        last_stamp = stamp;
                    }

                    free(payload_buf);
                    cur_rp += cmd_len;
                    cmd_idx += 1;
                }

                uint32_t new_rp = cur_rp;
                if (p->dev->desc.shell.write_memory) {
                    p->dev->desc.shell.write_memory(
                        p->dev->desc.shell.opaque,
                        descr_gpa + 4u, sizeof(new_rp), &new_rp);
                }
                LAGFX_TRACE("doorbell ch=%u: display vchan drained "
                          "%u cmd(s), rp=%u->%u",
                          ch, cmd_idx, read_ptr, new_rp);
            }

            lagfx_advance_stamp_cell(p, ch, last_stamp);
            p->pending_displays_bitmask |= (1u << ch);
            if (p->dev && p->dev->desc.shell.raise_interrupt) {
                p->dev->desc.shell.raise_interrupt(
                    p->dev->desc.shell.opaque, 0u);
                p->interrupts_raised += 1;
                LAGFX_TRACE("doorbell ch=%u: display bit+IRQ "
                          "(display_mask=0x%08x)",
                          ch, p->pending_displays_bitmask);
            }
            return;
        }
        default: break;
    }
}

/* === Stats accessors ======================================== */

void lagfx_protocol_stats(const lagfx_protocol_t *p,
                          uint64_t *total_cmds_seen_out,
                          uint64_t *total_cmds_completed_out,
                          uint64_t *unknown_opcode_count_out) {
    if (!lagfx_protocol_is_valid(p)) {
        if (total_cmds_seen_out)      *total_cmds_seen_out = 0;
        if (total_cmds_completed_out) *total_cmds_completed_out = 0;
        if (unknown_opcode_count_out) *unknown_opcode_count_out = 0;
        return;
    }
    if (total_cmds_seen_out)      *total_cmds_seen_out = p->total_cmds_seen;
    if (total_cmds_completed_out) *total_cmds_completed_out = p->total_cmds_completed;
    if (unknown_opcode_count_out) *unknown_opcode_count_out = p->unknown_opcode_count;
}

uint32_t lagfx_protocol_last_completed_stamp(const lagfx_protocol_t *p) {
    return lagfx_protocol_is_valid(p) ? p->last_completed_stamp : 0u;
}

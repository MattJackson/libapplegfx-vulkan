/*
 * libapplegfx-vulkan — protocol decoder lifecycle + dispatcher
 * src/protocol/protocol.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Phase 1.A.2 scaffold. Provides:
 *
 *   - lagfx_protocol_{new,free,reset} lifecycle.
 *   - lagfx_protocol_mmio_{read,write} register shadow + setter
 *     candidate probe (doorbell offset is not yet identified).
 *   - lagfx_protocol_dispatch_one — parse 12-byte header, look up
 *     opcode, invoke handler, run completion path.
 *
 * The FIFO ring dequeue itself is stubbed in fifo.c (R1 gap).
 * Tests drive dispatch via lagfx_protocol_dispatch_one directly.
 */

#include "protocol.h"
#include "state.h"
#include "opcodes.h"
#include "fifo.h"
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

    p->total_cmds_seen      = 0;
    p->total_cmds_completed = 0;
    p->unknown_opcode_count = 0;
    p->interrupts_raised    = 0;
    p->last_completed_stamp = 0;
    p->last_setter_offset   = 0;
    p->last_setter_value    = 0;
    p->setter_write_count   = 0;
    p->read_ptr             = 0;
    p->write_ptr            = 0;
    p->pending_stamps_bitmask = 0;

    p->display_swaps_applied          = 0;
    p->display_transactions_submitted = 0;
    p->display_acks_received          = 0;

    /* Phase 3.A inner-opcode counters. */
    p->inner_opcodes_processed            = 0;
    p->inner_opcodes_bind_pipeline        = 0;
    p->inner_opcodes_bind_vertex_buffer   = 0;
    p->inner_opcodes_bind_fragment_resource = 0;
    p->inner_opcodes_set_render_target    = 0;
    p->inner_opcodes_draw                 = 0;
    p->inner_opcodes_set_viewport         = 0;
    p->inner_opcodes_unknown              = 0;
}

/* === Completion path ========================================
 *
 * Every command unconditionally signals its stamp when done. Per
 * Per A4d (2026-04-24): the kext's unified ISR on MSI-X vec 0 reads
 * BAR0+0x1018 as a stamp-completion bitmask and feeds it to
 * AppleParavirtEventMachine::signalStamps. So our per-completion work
 * is to OR bit `stamp_id` into pending_stamps_bitmask and raise MSI-X.
 * The ISR loops set bits, calls commandWakeup per stamp, which reads
 * the actual stamp value from [EM+0x20] in kernel heap. No DMA of
 * stamp values by us is required. */

void lagfx_protocol_complete_stamp(lagfx_protocol_t *p, uint32_t stamp) {
    if (!lagfx_protocol_is_valid(p)) {
        return;
    }

    p->last_completed_stamp = stamp;
    p->total_cmds_completed += 1;

    /* Per RE session 2026-04-24 (post A6d+A6e+A6f + runtime dmesg):
     * for the RootChannel EventMachineFast2 with num_sids=32, the
     * allocStampTable non-short-path (at 0x14855c36) allocates a
     * 256-byte table of u32 pointers, each pointing at
     * `baseAddr + i*4` where baseAddr = [accel+0xe10] = FIFO IOBMD
     * first-page kernel VA. So stampBases[stamp_id] IS at
     * `(ring_base_pfn<<12) + stamp_id*4` in DMA-visible memory.
     *
     * A3f's earlier claim that num_sids==1 short-path stored baseAddr
     * at [EM+0x20] was technically correct for num_sids==1 but does
     * not apply to our runtime: dmesg confirms numStamps=32, not 1.
     *
     * The kext's checkGPUProgress / waitForStamp predicate reads:
     *   stamp = *stampBases[stamp_id] = u32 at FIFO_base + stamp_id*4
     * and compares to the target. With num_sids=32 the stamp cell
     * IS DMA-writable from the host.
     *
     * Empirical evidence: guest dmesg shows
     *   waitForStamp: timeout waiting for Apple Paravirt Accelerator
     *   stamp 7 (gpu_stamp=0)
     * i.e. the kext sees gpu_stamp=0 at FIFO+0 because we never
     * write there. Writing the command's stamp value (from the 12-
     * byte header) to FIFO + stamp_id*4 satisfies the predicate.
     *
     * Write target value + raise bitmask bit + MSI-X. */
    p->pending_stamps_bitmask |= (1u << 0);

    if (p->ring_base_pfn != 0u
        && p->dev && p->dev->desc.shell.write_memory) {
        /* stamp_id=0 for all RootChannel commands (confirmed A6e:
         * every init-phase command emits `xor esi, esi` before
         * writeStamp to pick slot 0). */
        unsigned stamp_id = 0u;
        uint64_t stamp_gpa = ((uint64_t)p->ring_base_pfn << 12)
                             + (uint64_t)stamp_id * 4u;
        if (p->dev->desc.shell.write_memory(
                p->dev->desc.shell.opaque,
                stamp_gpa,
                sizeof(stamp),
                &stamp)) {
            LAGFX_LOG("stamp_cell[%u] := 0x%08x (gpa=0x%llx)",
                      stamp_id, stamp,
                      (unsigned long long)stamp_gpa);
        }
    }

    if (p->dev && p->dev->desc.shell.raise_interrupt) {
        /* Per A4d (2026-04-24): the accelerator kext registers exactly
         * one interrupt source at MSI-X vector 0. A single unified ISR
         * demuxes stamps / displays / faults off BAR0 status regs. No
         * separate "stamp-interrupt" vector exists; vec 1 hits another
         * kext's ISR and breaks guest networking. Pin to vec 0. */
        p->dev->desc.shell.raise_interrupt(p->dev->desc.shell.opaque, 0u);
        p->interrupts_raised += 1;
        LAGFX_LOG("complete_stamp: cmd_stamp=0x%08x + IRQ vec=0 "
                  "(pending_mask=0x%08x)",
                  stamp, p->pending_stamps_bitmask);
    } else {
        LAGFX_LOG("complete_stamp: cmd_stamp=0x%08x (no IRQ cb)",
                  stamp);
    }
}

/* === Dispatcher ============================================ */

int lagfx_protocol_dispatch_one(lagfx_protocol_t *p,
                                const uint8_t *cmd_bytes,
                                size_t cmd_len) {
    if (!lagfx_protocol_is_valid(p) || !cmd_bytes) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }

    lagfx_cmd_header_t hdr;
    if (!lagfx_fifo_parse_header(cmd_bytes, cmd_len, &hdr)) {
        LAGFX_ERR("dispatch: header parse failed (len=%zu)", cmd_len);
        return LAGFX_HANDLER_ERR_SIZE;
    }

    p->total_cmds_seen += 1;

    const lagfx_op_descriptor_t *desc = lagfx_opcode_lookup(hdr.opcode);
    lagfx_op_handler_fn fn = (desc && desc->handler) ? desc->handler
                                                     : lagfx_op_default_handler;

    if (!desc) {
        p->unknown_opcode_count += 1;
    }

    LAGFX_LOG("dispatch: op=0x%04x (%s) stamp=0x%08x arg_count_8b=%u "
              "length=%u payload=%u",
              hdr.opcode, lagfx_opcode_name(hdr.opcode),
              hdr.stamp, (unsigned)hdr.arg_count_8b,
              (unsigned)hdr.length, (unsigned)hdr.payload_size);

    /* Payload-size guard. Handlers that want tighter validation do
     * their own extra checks. */
    if (desc) {
        if (hdr.payload_size < desc->min_payload) {
            LAGFX_WARN("dispatch: %s payload too small (%u < %u min)",
                       desc->name, (unsigned)hdr.payload_size,
                       (unsigned)desc->min_payload);
            /* Still run completion so guest doesn't hang. */
            lagfx_protocol_complete_stamp(p, hdr.stamp);
            return LAGFX_HANDLER_ERR_SIZE;
        }
        if (desc->max_payload != 0 && hdr.payload_size > desc->max_payload) {
            LAGFX_WARN("dispatch: %s payload too large (%u > %u max)",
                       desc->name, (unsigned)hdr.payload_size,
                       (unsigned)desc->max_payload);
            /* Fall through anyway — fail-open. */
        }
    }

    lagfx_handler_status_t rc = fn(p, &hdr);

    /* Completion path runs unconditionally — see note on
     * lagfx_protocol_complete_stamp. Even if the handler returned
     * an error we write the stamp per the fail-open note in §6.3. */
    lagfx_protocol_complete_stamp(p, hdr.stamp);

    return (int)rc;
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
        LAGFX_LOG("mmio_read: FIFO_FAULT_OFFSET -> 0x%x (read_ptr)",
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
        LAGFX_LOG("mmio_read: 0x102c (fault_status) -> 0");
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
        LAGFX_LOG("mmio_read: 0x1018 (stamp_bitmask) xchg -> 0x%x", mask);
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
        LAGFX_LOG("mmio_read: 0x1014 (display_bitmask) xchg -> 0x%x", mask);
        return mask;
    }

    int idx = lagfx_protocol_reg_index(offset);
    if (idx < 0) {
        LAGFX_LOG("mmio_read: unmapped offset 0x%llx -> 0",
                  (unsigned long long)offset);
        return 0;
    }

    uint32_t value = p->reg[idx];

    LAGFX_LOG("mmio_read: off=0x%llx -> 0x%08x",
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
        LAGFX_LOG("mmio_write: unmapped offset 0x%llx val=0x%08x",
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

    LAGFX_LOG("mmio_write: off=0x%llx val=0x%08x",
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
            LAGFX_LOG("mmio_write: STATUS_CONTROL ring_armed=%d",
                      (int)p->ring_armed);
            return;
        case 0x1004u:
            p->ring_size = value ? value : 0x10000u;
            LAGFX_LOG("mmio_write: ring_size=0x%x", p->ring_size);
            return;
        case 0x1008u: {
            uint32_t old_wp = p->write_ptr;
            p->write_ptr = value;
            LAGFX_LOG("mmio_write: doorbell wp=0x%x (was 0x%x)",
                      value, old_wp);
            if (p->ring_armed && p->write_ptr != p->read_ptr) {
                size_t drained = lagfx_fifo_drain(p);
                LAGFX_LOG("mmio_write: doorbell drained %zu cmds", drained);
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
            LAGFX_LOG("mmio_write: ring_start_offset=0x%x -> gpa=0x%llx",
                      value, (unsigned long long)p->ring_base_gpa);
            return;
        case 0x101cu:
            p->ring_shared_page_pfn = value;
            LAGFX_LOG("mmio_write: ring_shared_page_pfn=0x%x", value);
            return;
        case 0x1030u: {
            p->ring_base_pfn = value;
            p->ring_base_gpa =
                ((uint64_t)value << 12) + p->ring_start_offset;
            if (p->ring_size == 0u) {
                p->ring_size = 0x10000u;
            }
            LAGFX_LOG("mmio_write: ring_base_pfn=0x%x -> gpa=0x%llx size=0x%x",
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
                LAGFX_LOG("doorbell ch=%u: out of range", ch);
                return;
            }
            if (p->ring_shared_page_pfn == 0u
                || !p->dev || !p->dev->desc.shell.read_memory) {
                LAGFX_LOG("doorbell ch=%u: shared page unknown or no read cb",
                          ch);
                return;
            }

            uint64_t shared_gpa = (uint64_t)p->ring_shared_page_pfn << 12;
            uint64_t descr_gpa = shared_gpa + 0x400u + 20u * (ch - 1u);
            uint8_t descr[20] = {0};
            if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                                  descr_gpa,
                                                  sizeof(descr), descr)) {
                LAGFX_LOG("doorbell ch=%u: read_memory failed at gpa=0x%llx",
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

            LAGFX_LOG("doorbell ch=%u: descr wr=%u rd=%u mid=0x%x "
                      "chan_id=%u ring_pfn=0x%x",
                      ch, write_ptr, read_ptr, mid, chan_id, ring_pfn);

            /* Sanity: descriptor's chan_id should match the value the kext
             * wrote to 0x1020. If not, descriptor format isn't what we expect
             * for this channel — bail rather than write to wrong location. */
            if (chan_id != ch) {
                LAGFX_LOG("doorbell ch=%u: chan_id mismatch "
                          "(descr says %u), aborting", ch, chan_id);
                return;
            }

            /* Drain if there's pending work and ring is mapped. */
            uint32_t cmd_stm = 0u;  /* Captured for stamp-cell write below. */
            uint32_t cmd_display_index = 0u;  /* From cmd payload, used for SS port. */
            uint32_t cmd_ss_pfn = 0u;          /* From cmd payload, BMD shared-state PFN. */
            int have_payload = 0;
            if (ring_pfn != 0u && write_ptr > read_ptr
                && write_ptr <= 0x100000u) {
                uint64_t ring_gpa = ((uint64_t)ring_pfn << 12);
                uint8_t hdr[12] = {0};
                if (p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                                     ring_gpa + read_ptr,
                                                     12, hdr)) {
                    uint16_t opcode  = (uint16_t)(hdr[0] | (hdr[1] << 8));
                    uint32_t cmd_len =
                        (uint32_t)(hdr[4] | (hdr[5] << 8)
                                   | (hdr[6] << 16) | (hdr[7] << 24));
                    cmd_stm =
                        (uint32_t)(hdr[8] | (hdr[9] << 8)
                                   | (hdr[10] << 16) | (hdr[11] << 24));
                    LAGFX_LOG("doorbell ch=%u: drain opcode=0x%04x len=%u "
                              "stamp=%u (gpa=0x%llx)",
                              ch, opcode, cmd_len, cmd_stm,
                              (unsigned long long)(ring_gpa + read_ptr));
                }

                /* Per AppleParavirtDisplayPipe-setupSharedState.annotated.asm
                 * +0x217..+0x230: setupSharedState appends an 8-byte cmd
                 * payload after the 12-byte header containing
                 *   { u32 display_index, u32 ss_pfn }
                 * where ss_pfn is the per-display BMD shared-state physical
                 * PFN (this is DIFFERENT from ring_pfn — the ring_pfn is the
                 * channel's command ring; the ss_pfn is the 4 KiB shared-
                 * state page allocated by setupSharedState's
                 * IOBufferMemoryDescriptor at this+0x368). The host needs
                 * ss_pfn to address the BMD page where the post-wait
                 * predicate lives (ss[0x12] = port). Total Display0 cmd is
                 * 20 bytes (write_ptr=0x14 in descriptor), matching this
                 * layout.
                 *
                 * Read the 8-byte payload at ring_gpa + read_ptr + 12. */
                if (write_ptr - read_ptr >= 20u) {
                    uint8_t pl[8] = {0};
                    if (p->dev->desc.shell.read_memory(
                            p->dev->desc.shell.opaque,
                            ring_gpa + read_ptr + 12u, 8, pl)) {
                        cmd_display_index =
                            (uint32_t)(pl[0] | (pl[1] << 8)
                                       | (pl[2] << 16) | (pl[3] << 24));
                        cmd_ss_pfn =
                            (uint32_t)(pl[4] | (pl[5] << 8)
                                       | (pl[6] << 16) | (pl[7] << 24));
                        have_payload = 1;
                        LAGFX_LOG("doorbell ch=%u: cmd payload "
                                  "display_index=%u ss_pfn=0x%x",
                                  ch, cmd_display_index, cmd_ss_pfn);
                    }
                }

                /* Advance read_ptr to write_ptr atomically. The kext's
                 * watchdog is polling read_ptr; once it reaches write_ptr
                 * the "GPU hang written:N read:0" log changes to read:N. */
                uint32_t new_read_ptr = write_ptr;
                if (p->dev->desc.shell.write_memory) {
                    if (p->dev->desc.shell.write_memory(
                            p->dev->desc.shell.opaque,
                            descr_gpa + 4u, sizeof(new_read_ptr),
                            &new_read_ptr)) {
                        LAGFX_LOG("doorbell ch=%u: descr.read_ptr 0x%x->0x%x",
                                  ch, read_ptr, new_read_ptr);
                    }
                }

                /* Write a stamp value to FIFO+ch*4 stamp cell.
                 * cmd_stm read from host-visible ring is consistently 0 because
                 * the actual kext cmd lives in kernel heap (see
                 * display0-cmd-actual-location.md). Default to 1 (the value
                 * the kext's GPU hang log shows as the expected target). */
                uint32_t stamp_value = (cmd_stm != 0u) ? cmd_stm : 1u;
                if (p->ring_base_pfn != 0u && p->dev->desc.shell.write_memory) {
                    uint64_t stamp_gpa =
                        ((uint64_t)p->ring_base_pfn << 12)
                        + (uint64_t)ch * 4u;
                    if (p->dev->desc.shell.write_memory(
                            p->dev->desc.shell.opaque,
                            stamp_gpa, sizeof(stamp_value), &stamp_value)) {
                        LAGFX_LOG("doorbell ch=%u: stamp_cell[%u] := %u "
                                  "(gpa=0x%llx)",
                                  ch, ch, stamp_value,
                                  (unsigned long long)stamp_gpa);
                    }
                }

                /* Per setupSharedState-post-wait-predicates.md (2026-04-25):
                 * after waitForStamp releases, the kext runs
                 *   apvAssert(fSharedState->port == fPort)
                 * at setupSharedState +0x2a8 where fPort = this->display_index
                 * (u32 at this+0x380) and fSharedState->port = u16 at the
                 * BMD shared-state page +0x12.
                 *
                 * CRITICAL: the BMD shared-state page is at
                 *   (cmd.ss_pfn << 12)
                 * NOT (ring_pfn << 12). ring_pfn is the per-channel command
                 * ring; ss_pfn is the per-display 4 KiB IOBufferMemoryDescriptor
                 * page allocated inside setupSharedState and sent to host via
                 * the 8-byte cmd payload. Writing to the wrong page is silent —
                 * the kext just sees a zero page and panics on the predicate
                 * (or, for Display0, accidentally passes because both pages
                 * happen to be zero, but the response code at +0x1c is also
                 * never seen).
                 *
                 * cmd_display_index from the payload is authoritative for the
                 * port value; ch-5 derivation is a fallback if the payload was
                 * not readable.
                 *
                 * Also write shared_state[0x1c] = 0 as the response code/host
                 * caps token (kext stashes it at this+0x384, no validation). */
                if (have_payload && cmd_ss_pfn != 0u
                    && p->dev->desc.shell.write_memory) {
                    uint64_t ss_gpa = ((uint64_t)cmd_ss_pfn << 12);

                    /* Required to satisfy the post-wait apvAssert. */
                    uint16_t port = (uint16_t)cmd_display_index;
                    if (p->dev->desc.shell.write_memory(
                            p->dev->desc.shell.opaque,
                            ss_gpa + 0x12u,
                            sizeof(port), &port)) {
                        LAGFX_LOG("doorbell ch=%u: ss[0x12] := %u "
                                  "(port, ss_gpa=0x%llx)",
                                  ch, (unsigned)port,
                                  (unsigned long long)(ss_gpa + 0x12u));
                    }

                    uint32_t resp_code = 0u;  /* ack ready / host caps token */
                    if (p->dev->desc.shell.write_memory(
                            p->dev->desc.shell.opaque,
                            ss_gpa + 0x1cu,
                            sizeof(resp_code), &resp_code)) {
                        LAGFX_LOG("doorbell ch=%u: ss[0x1c] := %u "
                                  "(ss_gpa=0x%llx)",
                                  ch, resp_code,
                                  (unsigned long long)(ss_gpa + 0x1cu));
                    }
                } else if (!have_payload) {
                    LAGFX_LOG("doorbell ch=%u: no cmd payload — skipping "
                              "ss[0x12]/ss[0x1c] write (would target wrong "
                              "page)", ch);
                }
            }

            /* Signal channel completion via the appropriate bitmask + IRQ.
             *
             * Per display0-cmd-actual-location.md (2026-04-25): display
             * channels (ch>=5) are routed through AppleParavirtDisplayMachine
             * (display bitmask at 0x1014, signalDisplays). Non-display child
             * channels (ch=1..4 = VirtualChannels) go through the stamp
             * bitmask at 0x1018 (signalStamps).
             *
             * Set the bit in BOTH bitmasks for display channels — the kext
             * reads each in its ISR demultiplexer, and the cost is only one
             * extra u32 OR. Either route may be the one the wrangler is
             * actually waiting on. */
            if (ch >= 5u) {
                p->pending_displays_bitmask |= (1u << ch);
            }
            p->pending_stamps_bitmask |= (1u << ch);
            if (p->dev && p->dev->desc.shell.raise_interrupt) {
                p->dev->desc.shell.raise_interrupt(
                    p->dev->desc.shell.opaque, 0u);
                p->interrupts_raised += 1;
                LAGFX_LOG("doorbell ch=%u: bit+IRQ "
                          "(pending_mask=0x%08x)",
                          ch, p->pending_stamps_bitmask);
            }
            return;
        }
        default: break;
    }

    /* Any remaining write in the setter-candidate range: log for probe
     * observability (0x1020, 0x1024, 0x1028, 0x1034 — still-unresolved
     * slots per Agent I's recipe). */
    if (offset >= LAGFX_REG_SETTER_CAND_FIRST &&
        offset <= LAGFX_REG_SETTER_CAND_LAST) {
        lagfx_fifo_on_mmio_setter(p, offset, value);
        return;
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

uint32_t lagfx_protocol_last_setter_offset(const lagfx_protocol_t *p) {
    return lagfx_protocol_is_valid(p) ? p->last_setter_offset : 0u;
}

uint32_t lagfx_protocol_last_setter_value(const lagfx_protocol_t *p) {
    return lagfx_protocol_is_valid(p) ? p->last_setter_value : 0u;
}

uint64_t lagfx_protocol_setter_write_count(const lagfx_protocol_t *p) {
    return lagfx_protocol_is_valid(p) ? p->setter_write_count : 0u;
}

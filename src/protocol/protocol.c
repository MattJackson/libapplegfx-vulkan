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
    p->root_stamp_counter   = 0;

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
 * re-followup-spec-gaps.md §5.1, the 12-byte header has no flags
 * field; the dylib's Cmd* handler tails always invoke the "signal
 * stamp" selector, which pushes the stamp into PGPendingStampQueue,
 * and the dequeue thread later writes the stamp into the host-to-
 * guest stamp cell (readable at MMIO 0x1014) and raises the IRQ.
 * We model that here by writing the stamp to the STAMP_CELL_1 shadow
 * and calling shell.raise_interrupt. */
/* Internal: push a stamp onto the pending queue. */
static void lagfx_pending_stamp_push(lagfx_protocol_t *p, uint32_t stamp) {
    uint32_t cap = (uint32_t)(sizeof(p->pending_stamps) /
                              sizeof(p->pending_stamps[0]));
    uint32_t next_head = (p->pending_stamps_head + 1u) % cap;
    if (next_head == p->pending_stamps_tail) {
        /* Queue full — drop oldest, keep newest. Should never happen
         * with the 128-cap and drain batch size of 128. */
        LAGFX_WARN("pending_stamp: queue full, dropping oldest stamp 0x%x",
                   p->pending_stamps[p->pending_stamps_tail]);
        p->pending_stamps_tail =
            (p->pending_stamps_tail + 1u) % cap;
    }
    p->pending_stamps[p->pending_stamps_head] = stamp;
    p->pending_stamps_head = next_head;
}

/* Internal: pop the next pending stamp; 0 if queue empty. */
static uint32_t lagfx_pending_stamp_pop(lagfx_protocol_t *p) {
    if (p->pending_stamps_head == p->pending_stamps_tail) {
        return 0u;
    }
    uint32_t cap = (uint32_t)(sizeof(p->pending_stamps) /
                              sizeof(p->pending_stamps[0]));
    uint32_t stamp = p->pending_stamps[p->pending_stamps_tail];
    p->pending_stamps_tail = (p->pending_stamps_tail + 1u) % cap;
    return stamp;
}

/* Internal: after the guest drains a stamp cell via xchg, check if
 * another pending stamp is waiting. If so, write it into the cell and
 * raise IRQ so the guest ISR runs again. */
static void lagfx_pending_stamp_advance(lagfx_protocol_t *p) {
    uint32_t next = lagfx_pending_stamp_pop(p);
    if (next == 0u) {
        return;
    }
    int c1 = lagfx_protocol_reg_index(LAGFX_REG_STAMP_CELL_1);
    int c2 = lagfx_protocol_reg_index(LAGFX_REG_STAMP_CELL_2);
    if (c1 >= 0) p->reg[c1] = next;
    if (c2 >= 0) p->reg[c2] = next;
    if (p->dev && p->dev->desc.shell.raise_interrupt) {
        p->dev->desc.shell.raise_interrupt(p->dev->desc.shell.opaque, 0);
        p->interrupts_raised += 1;
    }
    LAGFX_LOG("pending_stamp: advanced to 0x%x, IRQ re-raised", next);
}

void lagfx_protocol_complete_stamp(lagfx_protocol_t *p, uint32_t stamp) {
    if (!lagfx_protocol_is_valid(p)) {
        return;
    }

    p->last_completed_stamp = stamp;
    p->total_cmds_completed += 1;

    /* NEW (2026-04-23) — Stamp-page writeback for IOGPUFamily wait loops.
     *
     * Per A2.A RE of IOGraphicsAccelerator2's `[this+0x380][vtbl+0x188]`
     * wait primitive: the kext blocks until `stamp_page[stamp_id*4]`
     * reaches a monotonically-allocated target. The targets for the 7
     * M3 init-phase commands on the root ring (stamp_id=0) are 1..7.
     * Incrementing a counter per root-ring completion and DMA-writing
     * it to stamp_page[0] satisfies all 7 waits. Order matters: write
     * memory BEFORE raising MSI-X so the ISR sees the new value on the
     * first read. */
    if (p->ring_shared_page_pfn != 0u
        && p->dev && p->dev->desc.shell.write_memory) {
        p->root_stamp_counter += 1u;
        uint64_t stamp_gpa = ((uint64_t)p->ring_shared_page_pfn << 12);
        if (p->dev->desc.shell.write_memory(
                p->dev->desc.shell.opaque,
                stamp_gpa,
                sizeof(p->root_stamp_counter),
                &p->root_stamp_counter)) {
            LAGFX_LOG("stamp_page[0] := %u (gpa=0x%llx, cmd_stamp=0x%08x)",
                      p->root_stamp_counter,
                      (unsigned long long)stamp_gpa, stamp);
        } else {
            LAGFX_WARN("stamp_page[0] write FAILED (gpa=0x%llx)",
                       (unsigned long long)stamp_gpa);
        }
    }

    /* Legacy path — keep MMIO stamp cells updated too.
     * These may be a secondary channel or unused; left in place for
     * safety while we validate the stamp-page path. */
    int c1 = lagfx_protocol_reg_index(LAGFX_REG_STAMP_CELL_1);
    int c2 = lagfx_protocol_reg_index(LAGFX_REG_STAMP_CELL_2);

    if (c1 >= 0) p->reg[c1] = stamp;
    if (c2 >= 0) p->reg[c2] = stamp;

    lagfx_pending_stamp_push(p, stamp);

    if (p->dev && p->dev->desc.shell.raise_interrupt) {
        p->dev->desc.shell.raise_interrupt(p->dev->desc.shell.opaque, 0);
        p->interrupts_raised += 1;
        LAGFX_LOG("complete_stamp: cmd_stamp=0x%08x written + IRQ raised "
                  "(root_counter=%u)", stamp, p->root_stamp_counter);
    } else {
        LAGFX_LOG("complete_stamp: cmd_stamp=0x%08x written (no IRQ cb)",
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
     * 0x102c — "Version/status query" per re-followup-spec-gaps.md §5.2.
     * Dylib's read dispatch forwards to an ObjC method returning u32.
     * Empirically the kext polls this after each stamp read (0x1014/
     * 0x1018). Returning 0 makes the kext wait indefinitely past M3;
     * trying last_completed_stamp to signal "we have status/work
     * available" — value doesn't need to be semantically perfect,
     * just non-zero and consistent.
     */
    if (offset == 0x102cu) {
        LAGFX_LOG("mmio_read: 0x102c -> 0x%x (last_stamp)",
                  p->last_completed_stamp);
        return p->last_completed_stamp;
    }

    /*
     * 0x1014 / 0x1018 — stamp cells are ATOMIC XCHG (read-and-clear)
     * per re-followup-spec-gaps.md §5.2:
     *   `xorl %eax, %eax; xchgl %eax, 0xf8(%rdi)`  for 0x1014
     *   `xorl %eax, %eax; xchgl %eax, 0x1c0(%rdi)` for 0x1018
     * Guest reads the pending-stamp value and simultaneously zeroes
     * the latch. Plain "return reg[idx]" leaves stamp=7 visible
     * forever — kext sees "stamp 7 pending" indefinitely, never
     * advances past setupDeviceInfo even after we serviced the
     * response. Model the xchg-with-0 behavior here.
     */
    if (offset == LAGFX_REG_STAMP_CELL_1 || offset == LAGFX_REG_STAMP_CELL_2) {
        int idx_xchg = lagfx_protocol_reg_index(offset);
        uint32_t prev = (idx_xchg >= 0) ? p->reg[idx_xchg] : 0u;
        if (idx_xchg >= 0) {
            p->reg[idx_xchg] = 0u;
        }
        LAGFX_LOG("mmio_read: stamp_cell 0x%llx xchg -> 0x%x (cleared)",
                  (unsigned long long)offset, prev);

        /* If CELL_1 was read (primary consume), advance to the next
         * pending stamp if any, raising another IRQ. This guarantees
         * every stamp the guest submitted gets individually delivered
         * even when multiple commands complete in one host drain. */
        if (offset == LAGFX_REG_STAMP_CELL_1) {
            lagfx_pending_stamp_advance(p);
        }
        return prev;
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

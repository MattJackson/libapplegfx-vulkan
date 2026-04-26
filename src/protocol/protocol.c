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
        LAGFX_LOG("complete_stamp[slot=%u]: cmd_stamp=0x%08x + IRQ vec=0 "
                  "(pending_mask=0x%08x)",
                  slot, stamp, p->pending_stamps_bitmask);
    } else {
        LAGFX_LOG("complete_stamp[slot=%u]: cmd_stamp=0x%08x (no IRQ cb)",
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

    LAGFX_LOG("dispatch: op=0x%04x (%s) stamp=0x%08x arg_count_8b=%u "
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

/* Translate a task-virtual address to a guest-physical address via
 * the per-task multi-level radix page-table.
 *
 * Page-table format (RE'd from `AppleParavirtTask::init` +
 * `PageTable::allocate` + `StorageNode::setEntry` 2026-04-26 evening,
 * see paravirt-re/agent-work/m4-cmdbuf-physical-hunt-progress.md):
 *
 *   Header at (root_page_pfn << 12):
 *     +0x00  u32 L1_pfn         (PFN of first interior node)
 *     +0x04  u32 levels         (= 3 for typical tasks)
 *     +0x08  u64 reserved0
 *     +0x10  u32 reserved1
 *     +0x14  u64 taskHandleHint
 *
 *   For levels=3, walk:
 *     shift = (levels-1)*10        (= 20 initially)
 *     node = L1_pfn
 *     while shift > 0:
 *         idx = (page_idx >> shift) & 0x3ff
 *         pte = u32 at (node<<12) + idx*4
 *         next = pte & 0x7fffffff
 *         is_leaf = pte >> 31     (interior=0, leaf=1)
 *         node = next
 *         shift -= 10
 *     leaf:
 *         leaf_idx = page_idx & 0x3ff
 *         pte = u32 at (node<<12) + leaf_idx*4
 *         data_pfn = pte & 0x7fffffff
 */
bool lagfx_task_translate(lagfx_protocol_t *p, uint32_t task_id,
                          uint64_t dev_addr, uint64_t *out_gpa,
                          uint64_t *out_run_len) {
    if (!lagfx_protocol_is_valid(p) || !out_gpa) {
        return false;
    }
    lagfx_task_entry_t *task = lagfx_protocol_find_task(p, task_id);
    if (!task) {
        return false;
    }
    if (!p->dev || !p->dev->desc.shell.read_memory) {
        return false;
    }

    /* First: check if the dev_addr falls inside one of the published
     * mapping intervals (from CmdMapMemoryImmediate 0x39). When the
     * scatter-block format is RE'd this will be the AUTHORITATIVE
     * lookup. For now we report the interval match in the log so we
     * can see which mappings are being hit. */
    for (unsigned i = 0; i < LAGFX_MAX_TASK_MAPPINGS; ++i) {
        const lagfx_task_mapping_t *m = &task->mappings[i];
        if (!m->live) continue;
        if (dev_addr < m->vaBase) continue;
        uint64_t off = dev_addr - m->vaBase;
        if (off >= m->length) continue;
        LAGFX_LOG("task_translate: taskID=%u dev=0x%llx -> mapping[%u] "
                  "vaBase=0x%llx length=0x%llx offset=0x%llx (scatter "
                  "decode TBD)",
                  task_id, (unsigned long long)dev_addr, i,
                  (unsigned long long)m->vaBase,
                  (unsigned long long)m->length,
                  (unsigned long long)off);
        /* TODO: once scatter blocks are decoded, look up segment and
         * return real GPA. For now, fall through to the radix walk so
         * the diagnostic chain stays consistent. */
        break;
    }

    if (task->root_page_pfn == 0u) {
        return false;
    }

    uint64_t header_gpa = ((uint64_t)task->root_page_pfn << 12);
    uint8_t hdr_bytes[8] = {0};
    if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                        header_gpa, sizeof(hdr_bytes),
                                        hdr_bytes)) {
        return false;
    }
    uint32_t l1_pfn = (uint32_t)(hdr_bytes[0]
                                 | ((uint32_t)hdr_bytes[1] << 8)
                                 | ((uint32_t)hdr_bytes[2] << 16)
                                 | ((uint32_t)hdr_bytes[3] << 24));
    uint32_t levels = (uint32_t)(hdr_bytes[4]
                                 | ((uint32_t)hdr_bytes[5] << 8)
                                 | ((uint32_t)hdr_bytes[6] << 16)
                                 | ((uint32_t)hdr_bytes[7] << 24));

    if (l1_pfn == 0u || levels == 0u || levels > 4u) {
        LAGFX_LOG("task_translate: taskID=%u dev=0x%llx root_pfn=0x%x "
                  "header invalid (l1_pfn=0x%x levels=%u)",
                  task_id, (unsigned long long)dev_addr,
                  task->root_page_pfn, l1_pfn, levels);
        return false;
    }

    uint64_t page_idx = dev_addr >> 12;
    uint32_t node_pfn = l1_pfn;
    int32_t shift = (int32_t)((levels - 1u) * 10u);

    while (shift > 0) {
        uint64_t idx = (page_idx >> shift) & 0x3ffu;
        uint64_t pte_gpa = ((uint64_t)node_pfn << 12) + idx * 4u;
        uint32_t pte = 0u;
        if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                            pte_gpa, sizeof(pte), &pte)) {
            return false;
        }
        if (pte == 0u) {
            LAGFX_LOG("task_translate: taskID=%u dev=0x%llx root=0x%x "
                      "l1=0x%x — unmapped at level shift=%d idx=%u "
                      "(node_pfn=0x%x pte_gpa=0x%llx)",
                      task_id, (unsigned long long)dev_addr,
                      task->root_page_pfn, l1_pfn, shift,
                      (unsigned)idx, node_pfn,
                      (unsigned long long)pte_gpa);
            return false;
        }
        node_pfn = pte & 0x7fffffffu;
        shift -= 10;
    }

    uint64_t leaf_idx = page_idx & 0x3ffu;
    uint64_t leaf_pte_gpa = ((uint64_t)node_pfn << 12) + leaf_idx * 4u;
    uint32_t leaf_pte = 0u;
    if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                        leaf_pte_gpa, sizeof(leaf_pte),
                                        &leaf_pte)) {
        return false;
    }
    if (leaf_pte == 0u) {
        LAGFX_LOG("task_translate: taskID=%u dev=0x%llx root=0x%x l1=0x%x "
                  "— leaf PTE zero (leaf_pte_gpa=0x%llx leaf_idx=%u "
                  "leaf_node_pfn=0x%x)",
                  task_id, (unsigned long long)dev_addr,
                  task->root_page_pfn, l1_pfn,
                  (unsigned long long)leaf_pte_gpa,
                  (unsigned)leaf_idx, node_pfn);
        return false;
    }
    uint32_t data_pfn = leaf_pte & 0x7fffffffu;

    uint64_t page_off = dev_addr & 0xfffu;
    *out_gpa = ((uint64_t)data_pfn << 12) + page_off;
    if (out_run_len) {
        *out_run_len = (uint64_t)0x1000u - page_off;
    }
    LAGFX_LOG("task_translate: taskID=%u dev=0x%llx root=0x%x l1=0x%x "
              "levels=%u data_pfn=0x%x -> gpa=0x%llx run=%llu",
              task_id, (unsigned long long)dev_addr,
              task->root_page_pfn, l1_pfn, levels, data_pfn,
              (unsigned long long)(*out_gpa),
              (unsigned long long)((out_run_len) ? *out_run_len : 0));
    return true;
}

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
                uint32_t data_pfn = 0u;
                p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                               ring_gpa_base, sizeof(data_pfn),
                                               &data_pfn);
                uint64_t ring_data_gpa = (data_pfn != 0u)
                    ? ((uint64_t)data_pfn << 12)
                    : ring_gpa_base + 0x1000u;

                uint32_t last_stamp = 0u;
                uint32_t cur_rp = read_ptr;
                unsigned cmd_idx = 0;
                while (cur_rp + 12u <= write_ptr) {
                    /* Read the 12-byte cmd header. */
                    uint8_t hdr_bytes[12];
                    if (!p->dev->desc.shell.read_memory(
                            p->dev->desc.shell.opaque,
                            ring_data_gpa + cur_rp, 12, hdr_bytes)) {
                        LAGFX_WARN("doorbell ch=%u: read_memory failed for "
                                   "cmd header at gpa=0x%llx",
                                   ch,
                                   (unsigned long long)(ring_data_gpa + cur_rp));
                        break;
                    }
                    uint32_t cmd_len = (uint32_t)(hdr_bytes[4]
                                                  | (hdr_bytes[5] << 8)
                                                  | (hdr_bytes[6] << 16)
                                                  | (hdr_bytes[7] << 24));
                    if (cmd_len < 12u || cur_rp + cmd_len > write_ptr) {
                        LAGFX_WARN("doorbell ch=%u: bad cmd_len=%u at rp=%u "
                                   "(wp=%u) — stopping walk",
                                   ch, cmd_len, cur_rp, write_ptr);
                        break;
                    }

                    /* Read the full cmd into a temporary buffer + dispatch. */
                    uint8_t *cmd = malloc(cmd_len);
                    if (!cmd) {
                        LAGFX_WARN("doorbell ch=%u: malloc(%u) failed",
                                   ch, cmd_len);
                        break;
                    }
                    if (!p->dev->desc.shell.read_memory(
                            p->dev->desc.shell.opaque,
                            ring_data_gpa + cur_rp, cmd_len, cmd)) {
                        LAGFX_WARN("doorbell ch=%u: read_memory(cmd, %u) "
                                   "failed", ch, cmd_len);
                        free(cmd);
                        break;
                    }

                    lagfx_cmd_header_t parsed;
                    int rc = lagfx_protocol_dispatch_one_no_stamp(
                        p, cmd, cmd_len, &parsed);
                    LAGFX_LOG("doorbell ch=%u cmd[%u]: opcode=0x%04x "
                              "stamp=0x%08x len=%u rc=%d",
                              ch, cmd_idx, parsed.opcode, parsed.stamp,
                              cmd_len, rc);

                    last_stamp = parsed.stamp;
                    free(cmd);

                    cur_rp += cmd_len;
                    cmd_idx += 1;
                }

                /* Advance descr.read_ptr to write_ptr (drain done). */
                uint32_t new_rp = write_ptr;
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
                LAGFX_LOG("doorbell ch=%u: drained %u cmd(s), rp=%u->%u, "
                          "stamp_cell+IRQ (pending_mask=0x%08x)",
                          ch, cmd_idx, read_ptr, new_rp,
                          p->pending_stamps_bitmask);
                return;
            }

            /* Drain if there's pending work and ring is mapped. */
            uint32_t cmd_stm = 0u;  /* Captured for stamp-cell write below. */
            uint32_t cmd_display_index = 0u;  /* From cmd payload, used for SS port. */
            uint32_t cmd_ss_pfn = 0u;          /* From cmd payload, BMD shared-state PFN. */
            int have_payload = 0;
            if (ring_pfn != 0u && write_ptr > read_ptr
                && write_ptr <= 0x100000u) {
                /* Per AppleParavirtVirtualChannel-init.annotated.asm BLOCK
                 * F+G+J: the per-channel ring BMD is allocated as
                 * inTaskWithOptions(capacity=ring_size+0x1000). The first
                 * page (page0) is a u32 PFN-array: page0[i] = physical
                 * page number of the i-th data page (ring_kva + 0x1000 +
                 * i*0x1000). this[+0x228] (the kva submitBuffer's memcpy
                 * targets) = map_kva + 0x1000.
                 *
                 * Older code used (ring_pfn<<12) + 0x1000 + read_ptr —
                 * which only works when the kernel allocator happens to
                 * give physically-contiguous pages. On a fragmented heap
                 * (observed 2026-04-26 ch=6 run: ring_pfn=0x3da1e3 but
                 * data page = 0x3da724) the +0x1000 bytes contain
                 * zeros (next BMD metadata page), the cmd payload reads
                 * back as zero, ss_pfn=0, and the post-wait apvAssert
                 * panics at AppleParavirtDisplayPipe.cpp:240.
                 *
                 * Correct path: read u32 at (ring_pfn<<12)+0 to get the
                 * actual data PFN (page0[0]), then read cmd at
                 * (data_pfn<<12) + read_ptr. The init pages of every
                 * channel cmd are <0x1000 bytes so we never cross a
                 * page boundary in the first cmd; for larger commands
                 * walking would index page0[read_ptr / 0x1000] (M4).
                 *
                 * Fallback for safety: if page0[0] reads zero, fall
                 * back to the +0x1000 contiguous-allocator heuristic so
                 * we don't break the (likely common) lucky-allocator
                 * path. */
                uint64_t ring_gpa_base = ((uint64_t)ring_pfn << 12);
                uint32_t data_pfn = 0u;
                p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                               ring_gpa_base, sizeof(data_pfn),
                                               &data_pfn);
                uint64_t ring_data_gpa;
                if (data_pfn != 0u) {
                    ring_data_gpa = (uint64_t)data_pfn << 12;
                } else {
                    /* Fallback: contiguous-allocator heuristic. */
                    ring_data_gpa = ring_gpa_base + 0x1000u;
                }
                LAGFX_LOG("doorbell ch=%u: data via page0 PFN=0x%x "
                          "(data_gpa=0x%llx)",
                          ch, data_pfn,
                          (unsigned long long)ring_data_gpa);

                /* Diagnostic 1: dump 32 bytes at ring_gpa_base + 0 (the
                 * "first page" — should be PFN-array OR header per init
                 * annotation, NOT cmd data). */
                {
                    uint8_t dbg[32] = {0};
                    if (p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                                         ring_gpa_base,
                                                         32, dbg)) {
                        LAGFX_LOG("doorbell ch=%u: dbg page0[0..31] = "
                                  "%02x %02x %02x %02x %02x %02x %02x %02x "
                                  "%02x %02x %02x %02x %02x %02x %02x %02x "
                                  "%02x %02x %02x %02x %02x %02x %02x %02x "
                                  "%02x %02x %02x %02x %02x %02x %02x %02x",
                                  ch,
                                  dbg[0], dbg[1], dbg[2], dbg[3],
                                  dbg[4], dbg[5], dbg[6], dbg[7],
                                  dbg[8], dbg[9], dbg[10], dbg[11],
                                  dbg[12], dbg[13], dbg[14], dbg[15],
                                  dbg[16], dbg[17], dbg[18], dbg[19],
                                  dbg[20], dbg[21], dbg[22], dbg[23],
                                  dbg[24], dbg[25], dbg[26], dbg[27],
                                  dbg[28], dbg[29], dbg[30], dbg[31]);
                    }
                }

                /* Diagnostic 2: dump 32 bytes at ring_data_gpa + read_ptr
                 * (the corrected position — should hold the 12-byte cmd
                 * header followed by the 8-byte payload). */
                {
                    uint8_t dbg[32] = {0};
                    if (p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                                         ring_data_gpa + read_ptr,
                                                         32, dbg)) {
                        LAGFX_LOG("doorbell ch=%u: dbg data+rp[0..31] = "
                                  "%02x %02x %02x %02x %02x %02x %02x %02x "
                                  "%02x %02x %02x %02x %02x %02x %02x %02x "
                                  "%02x %02x %02x %02x %02x %02x %02x %02x "
                                  "%02x %02x %02x %02x %02x %02x %02x %02x "
                                  "(gpa=0x%llx)",
                                  ch,
                                  dbg[0], dbg[1], dbg[2], dbg[3],
                                  dbg[4], dbg[5], dbg[6], dbg[7],
                                  dbg[8], dbg[9], dbg[10], dbg[11],
                                  dbg[12], dbg[13], dbg[14], dbg[15],
                                  dbg[16], dbg[17], dbg[18], dbg[19],
                                  dbg[20], dbg[21], dbg[22], dbg[23],
                                  dbg[24], dbg[25], dbg[26], dbg[27],
                                  dbg[28], dbg[29], dbg[30], dbg[31],
                                  (unsigned long long)(ring_data_gpa + read_ptr));
                    }
                }

                /* Real header read — corrected to use ring_data_gpa. */
                uint8_t hdr[12] = {0};
                if (p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                                     ring_data_gpa + read_ptr,
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
                              (unsigned long long)(ring_data_gpa + read_ptr));
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
                 * Read the 8-byte payload at ring_data_gpa + read_ptr + 12
                 * (i.e. (ring_pfn<<12) + 0x1000 + read_ptr + 12). */
                if (write_ptr - read_ptr >= 20u) {
                    uint8_t pl[8] = {0};
                    if (p->dev->desc.shell.read_memory(
                            p->dev->desc.shell.opaque,
                            ring_data_gpa + read_ptr + 12u, 8, pl)) {
                        cmd_display_index =
                            (uint32_t)(pl[0] | (pl[1] << 8)
                                       | (pl[2] << 16) | (pl[3] << 24));
                        cmd_ss_pfn =
                            (uint32_t)(pl[4] | (pl[5] << 8)
                                       | (pl[6] << 16) | (pl[7] << 24));
                        have_payload = 1;
                        LAGFX_LOG("doorbell ch=%u: cmd payload "
                                  "raw=%02x %02x %02x %02x %02x %02x %02x %02x "
                                  "display_index=%u ss_pfn=0x%x "
                                  "(gpa=0x%llx)",
                                  ch,
                                  pl[0], pl[1], pl[2], pl[3],
                                  pl[4], pl[5], pl[6], pl[7],
                                  cmd_display_index, cmd_ss_pfn,
                                  (unsigned long long)(ring_data_gpa + read_ptr + 12u));
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

                /* Write a stamp value to FIFO+ch*4 stamp cell via the
                 * monotonic helper. cmd_stm from host-visible ring is
                 * consistently 0 because the actual kext cmd lives in
                 * kernel heap (display0-cmd-actual-location.md); the
                 * helper's max(target, cur+1) clamp guarantees the cell
                 * advances regardless. Slot index = ch (display channels
                 * have slot == chan_id; ring_base_pfn is the FIFO data
                 * page from BAR0+0x1030). */
                lagfx_advance_stamp_cell(p, ch, cmd_stm);

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
                } else {
                    /* have_payload=1 but cmd_ss_pfn==0 — ring read returned
                     * zero bytes. Per display0-cmd-actual-location.md, the
                     * kext writes the cmd body via wrapper::appendBytes()
                     * into a stack buffer that gets submitted; observed
                     * behaviour is that (ring_pfn<<12)+read_ptr is NOT
                     * coherent with the kext's writes (the page is either
                     * a metadata/decorative slot or the data lives in
                     * kernel heap). We can derive display_index from the
                     * channel number (display_index = ch - 5) but without
                     * ss_pfn we cannot address the per-display BMD page,
                     * so the post-wait apvAssert
                     *   (fSharedState->port == fPort)
                     * will pass for ch=5 (Display0, fPort=0, page bzero'd)
                     * but PANIC for ch>=6 at AppleParavirtDisplayPipe.cpp:240.
                     * Logged here so the diagnostic trail is visible. */
                    unsigned derived_display_index =
                        (ch >= 5u) ? (ch - 5u) : 0u;
                    LAGFX_LOG("doorbell ch=%u: cmd_ss_pfn=0 from ring read "
                              "(derived display_index=%u) — cannot address "
                              "BMD ss page; predicate will panic for ch>=6",
                              ch, derived_display_index);
                }
            }

            /* Signal stamp completion to wake the setupSharedState
             * waitForStamp parker. The stamp bitmask uses bit `ch` (the
             * channel number = display_index+5 for Display0/1/2). */
            p->pending_stamps_bitmask |= (1u << ch);
            if (p->dev && p->dev->desc.shell.raise_interrupt) {
                p->dev->desc.shell.raise_interrupt(
                    p->dev->desc.shell.opaque, 0u);
                p->interrupts_raised += 1;
                LAGFX_LOG("doorbell ch=%u: stamp bit+IRQ "
                          "(pending_mask=0x%08x)",
                          ch, p->pending_stamps_bitmask);
            }

            /* M3 attempt 3 — wake AppleParavirtDisplayPipe::process_online
             * by populating the rest of shared_state and raising the
             * display-online IRQ. Without this the kext finishes
             * setupSharedState successfully, then sits forever in the
             * IES action waiting for the host to advertise display online.
             *
             * Bitmask routing: AppleParavirtDisplayMachine::signalDisplays
             * does `bsf` over the mask and calls
             * `getDisplayPipe(bit)->signalDisplay()`. getDisplayPipe at
             * IOAccel+0xe344 indexes pipes by display_index
             * (`[dm+0x88+idx*8]`), so the bit number MUST equal
             * display_index (0/1/2), NOT the channel id (5/6/7).
             *
             * process_online (kext+0xb48a) reads the following ss fields
             * before forwarding to AppleParavirtFramebuffer::connectionChange:
             *   ss[0x00]  u32 connection-id (mode-id)
             *   ss[0x04+] cstring modeName (we leave zero-terminated empty)
             *   ss[0x14]  u16 active width
             *   ss[0x16]  u16 active height
             *   ss[0x18]  u16 cursor max width
             *   ss[0x1a]  u16 cursor max height
             *   ss[0x20]  u32 framebuffer aperture length (cookie)
             *   ss[0x2c..0x48] 8*f32 colorimetry (sRGB primaries + D65
             *                  whitepoint) — kext wrote defaults pre-stamp
             *                  so we leave them.
             *   ss[0x4c]  u16 hSyncTotal/DPI
             *   ss[0x4e]  u16 vSyncTotal/DPI — kext wrote 0xFFFF defaults
             *   ss[0x50]  u8  flags
             *   ss[0x200] u32 framebuffer aperture PFN
             *   ss[0x208] u16 numPixelFormats
             *   ss[0x210+i*16] per-format records — ignored at numFormats=0
             *
             * For M3 we publish a synthetic 1920x1080 BGRA8 mode and zero
             * pixel-format records (the format query block at
             * 0x1456182c is only invoked if numPixelFormats > 0). */
            if (ch >= 5u && have_payload && cmd_ss_pfn != 0u
                && p->dev && p->dev->desc.shell.write_memory) {
                uint64_t ss_gpa = ((uint64_t)cmd_ss_pfn << 12);
                uint32_t display_index = cmd_display_index;

                /* ss[0x00]: connection-id (mode-id 1 = synthetic mode). */
                uint32_t conn_id = 1u;
                p->dev->desc.shell.write_memory(
                    p->dev->desc.shell.opaque, ss_gpa + 0x00u,
                    sizeof(conn_id), &conn_id);

                /* ss[0x14]: width, ss[0x16]: height (both u16). */
                uint16_t width = 1920u;
                uint16_t height = 1080u;
                p->dev->desc.shell.write_memory(
                    p->dev->desc.shell.opaque, ss_gpa + 0x14u,
                    sizeof(width), &width);
                p->dev->desc.shell.write_memory(
                    p->dev->desc.shell.opaque, ss_gpa + 0x16u,
                    sizeof(height), &height);

                /* ss[0x18..0x1a]: cursor glyph max (64x64 covers both
                 * the 32-bit MTL hardware cursor max and the 64-bit
                 * software cursor). */
                uint16_t cursor_max = 64u;
                p->dev->desc.shell.write_memory(
                    p->dev->desc.shell.opaque, ss_gpa + 0x18u,
                    sizeof(cursor_max), &cursor_max);
                p->dev->desc.shell.write_memory(
                    p->dev->desc.shell.opaque, ss_gpa + 0x1au,
                    sizeof(cursor_max), &cursor_max);

                /* ss[0x20]: FB aperture length (cookie). 8 MB suffices
                 * for 1920x1080xBGRA8 (~8.3 MB). */
                uint32_t fb_len = 1920u * 1080u * 4u;
                p->dev->desc.shell.write_memory(
                    p->dev->desc.shell.opaque, ss_gpa + 0x20u,
                    sizeof(fb_len), &fb_len);

                /* ss[0x50]: orientation flags (0 = normal). */
                uint8_t orient = 0u;
                p->dev->desc.shell.write_memory(
                    p->dev->desc.shell.opaque, ss_gpa + 0x50u,
                    sizeof(orient), &orient);

                /* ss[0x200]: FB aperture PFN. We use ss_pfn itself as a
                 * placeholder; the kext caches this as a token at
                 * this+0x3ac and forwards to connectionChange. Real FB
                 * aperture binding happens via a later opcode. */
                uint32_t fb_pfn = cmd_ss_pfn;
                p->dev->desc.shell.write_memory(
                    p->dev->desc.shell.opaque, ss_gpa + 0x200u,
                    sizeof(fb_pfn), &fb_pfn);

                /* ss[0x208]: numPixelFormats = 0. The block_invoke at
                 * 0x1456182c only runs if this is >0. */
                uint16_t num_formats = 0u;
                p->dev->desc.shell.write_memory(
                    p->dev->desc.shell.opaque, ss_gpa + 0x208u,
                    sizeof(num_formats), &num_formats);

                LAGFX_LOG("doorbell ch=%u: populated ss for online "
                          "(display_index=%u, %ux%u@1mode_id, ss_gpa=0x%llx)",
                          ch, display_index, width, height,
                          (unsigned long long)ss_gpa);

                /* Now raise display-online IRQ. Bit position MUST be
                 * display_index (0/1/2), NOT channel (5/6/7) — verified
                 * via IOAccelDisplayMachine::getDisplayPipe disasm. */
                p->pending_displays_bitmask |= (1u << display_index);
                if (p->dev->desc.shell.raise_interrupt) {
                    p->dev->desc.shell.raise_interrupt(
                        p->dev->desc.shell.opaque, 0u);
                    p->interrupts_raised += 1;
                    LAGFX_LOG("doorbell ch=%u: display-online bit+IRQ "
                              "(display_index=%u, "
                              "display_pending_mask=0x%08x)",
                              ch, display_index,
                              p->pending_displays_bitmask);
                }
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

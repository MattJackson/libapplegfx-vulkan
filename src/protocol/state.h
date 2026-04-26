/*
 * libapplegfx-vulkan — protocol decoder internal state
 * src/protocol/state.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Layout of the opaque lagfx_protocol_t struct. Used by protocol.c,
 * fifo.c, ops_*.c. Tests poke via the public accessors in
 * protocol.h. Private to src/protocol/.
 */

#ifndef LIBAPPLEGFX_PROTOCOL_STATE_H
#define LIBAPPLEGFX_PROTOCOL_STATE_H

#include "protocol.h"
#include "opcodes.h"

#include <stdbool.h>
#include <stdint.h>

/* Table capacities — see phase-1a2-decoder-plan.md §5 and
 * phase-2-first-pixel-plan.md §4 (Phase 2.A display-path opcodes add
 * the displays[] table). */
#define LAGFX_MAX_TASKS       16u
#define LAGFX_MAX_CHILDFIFOS   8u
#define LAGFX_MAX_INFLIGHT    32u
#define LAGFX_PROTO_MAX_DISPLAYS 4u  /* tracks guest-visible displayID →
                                      * current mapping + last txn.
                                      * Matches device.h's cap; kept as a
                                      * separate constant because state.h
                                      * intentionally doesn't pull
                                      * device.h (avoids circular-ish
                                      * reach). */

/* Magic cookie for liveness checks; ASCII "LAPR" (Linux Apple PRotocol). */
#define LAGFX_PROTOCOL_MAGIC  0x4C415052u

/* lagfx_task_t is opaque to us at this layer; we hold a pointer only.
 * (Defined in libapplegfx-vulkan.h as an incomplete type.) */
typedef struct lagfx_task lagfx_task_t;

/* Per-task VA → guest-physical mapping interval, populated by
 * CmdMapMemoryImmediate (kext opcode 0x39, ch=2 Immediate vchan).
 * `host_gpu_addr` values in CmdExecIndirect2 resource_table entries
 * are guest-kernel VAs that fall inside one of these intervals.
 *
 * For now the intervals only carry {vaBase, length}; the actual GPA
 * mapping for a given offset within the interval is captured from
 * any preceding scatter blocks in the 0x39 payload (TODO — boot-trace
 * 0x39s have zero-length scatter, so we don't have a runtime sample
 * to RE the scatter format yet). */
typedef struct {
    uint64_t vaBase;     /* device-VA range start  */
    uint64_t length;     /* range length in bytes  */
    bool     live;
    /* TODO: once scatter-block format is RE'd, store
     *   uint32_t segment_count;
     *   lagfx_va_segment_t *segments;
     * for chunked GPA lookup. */
} lagfx_task_mapping_t;

#define LAGFX_MAX_TASK_MAPPINGS 32u

typedef struct {
    uint32_t      id;         /* taskID from CmdDefineTask2 */
    lagfx_task_t *shell_task; /* opaque handle from shell.create_task */
    uint64_t      base_va;    /* base of reserved VA range */
    uint64_t      length;
    bool          live;

    /* Host-task PFN-array indirection (M4):
     * CmdDefineHostTask (0x38) publishes a root_page_pfn whose first
     * page is the SHARED HEADER. See state-machines/per-task-page-table.md
     * for the actual radix-tree format. The kext also publishes
     * VA→GPA mappings via opcode 0x39 (CmdMapMemoryImmediate) — those
     * land in `mappings[]` below and are the AUTHORITATIVE translation
     * source for host_gpu_addr values. */
    uint32_t      root_page_pfn;

    /* Mappings from CmdMapMemoryImmediate (0x39). On segment-walker
     * lookup, scan for the first mapping containing dev_addr; if
     * found, the offset within (dev_addr - vaBase) tells us where in
     * the IOAccelSysMemory backing to read. Translation of offset →
     * GPA awaits scatter-block format RE. */
    lagfx_task_mapping_t mappings[LAGFX_MAX_TASK_MAPPINGS];
} lagfx_task_entry_t;

typedef struct {
    uint32_t id;
    uint64_t buffer_va;
    uint32_t size;
    bool     live;
    /* Phase 1.A.2 scaffold marker: set by CmdSynchronizeResources /
     * other barrier ops to indicate the resource has been quiesced.
     * Full sync semantics (per-resource barrier tracking) land in
     * Phase 3. */
    bool     synced;
} lagfx_childfifo_entry_t;

/* Per-task resource sync scaffold. Phase 1.A.2 tracks only a monotonically
 * increasing counter of successful syncs per task (so tests can observe
 * that CmdSynchronizeResources for a known taskID actually reached the
 * task). Full per-resource barrier tracking is Phase 3 work. */
typedef struct {
    uint32_t stamp;
    uint16_t opcode;
    bool     completed;
} lagfx_inflight_entry_t;

/* Display-pipe tracking (Phase 2.A). Populated by CmdDisplaySwapMapping
 * (0x12) and updated by CmdDisplayTransaction3 (0x16); cleared when the
 * guest issues the matching CmdDisplayAck (0x10).
 *
 * Field layouts are PARTIAL-confidence per phase-2-first-pixel-plan.md
 * §4 (runtime capture on a booted VM would definitively confirm). The
 * guest emits one of these per guest-visible display; scanout backing is
 * a guest VA into task-mapped memory that the host later resolves via
 * shell.read_memory or the direct host-addr from map_memory.
 *
 * In Phase 2, this table is the decoder's source of truth for
 *   "which framebuffer should the shell read_frame pull from?"
 * Phase 2.B.6 will extend this with a pointer to the host-side
 * lagfx_vk_render_target_t once that type exists.
 */
typedef struct {
    uint32_t id;                  /* displayID (opaque; guest-chosen)     */
    bool     live;                /* slot occupied                        */

    /* Current mapping (latest CmdDisplaySwapMapping). */
    uint64_t mapping_id;          /* monotonic per display; incremented
                                   * on every swap (PARTIAL — runtime
                                   * capture may reveal a more specific
                                   * encoding).                          */
    uint64_t buffer_va;           /* guest VA of scanout backing         */
    uint64_t length;              /* total bytes of scanout (0 if unknown) */
    uint32_t width;               /* px (0 if not carried on the wire)   */
    uint32_t height;              /* px                                   */
    uint32_t stride;              /* bytes per row                        */
    uint32_t format;              /* LAGFX/PVG format enum; 0=BGRA8_UNORM */
    bool     mapped;              /* a mapping has been registered        */

    /* Most recent transaction submitted against this display. Cleared
     * when the matching ack is received (tx_acked=true). */
    uint32_t pending_transaction_id;
    bool     transaction_pending;
    bool     transaction_acked;

    /* Summary of last transaction's first attachment, for Phase 2.A
     * clear-colour parse + Phase 2.B consumption. */
    uint32_t last_attachment_count;
    uint8_t  last_load_action;    /* 0=dontcare,1=load,2=clear (Metal)   */
    float    last_clear_rgba[4];

    /* Phase 3.A scaffold: last pipeline handle observed from a
     * CmdExecIndirect2 inner-opcode BIND_PIPELINE entry addressed to
     * this display. Cleared to 0 on reset. Not yet bound to a
     * VkShaderEXT — Phase 3.A.2 will translate this to
     * vkCmdBindShadersEXT against a VkCommandBuffer. */
    uint32_t last_pipeline;
} lagfx_display_entry_t;

struct lagfx_protocol {
    uint32_t magic;                 /* LAGFX_PROTOCOL_MAGIC */
    struct lagfx_device *dev;       /* back-pointer for shell callbacks */

    /* MMIO register shadow — 15 regs * 4 bytes, indexed by
     * (offset - LAGFX_REG_BASE) / 4. Covers 0x1000..0x1038. */
    uint32_t reg[LAGFX_REG_COUNT];

    /* Ring geometry (populated via MMIO setter writes in the
     * 0x1000..0x1034 bank, per paravirt-re/mmio-survival-recipe-v2.md):
     *   0x1010 W → page_size (expected 0x1000)
     *   0x101c W → ring_shared_page_pfn (shared/mailbox page)
     *   0x1030 W → ring_base_pfn (64 KiB command ring PFN)
     *   0x1000 W → kick / master enable (triggers drain)
     * ring_base_gpa is computed as ring_base_pfn << 12 once the page
     * size is known. */
    bool     ring_armed;
    uint32_t page_size;             /* assumed 4K */
    uint32_t ring_start_offset;     /* from 0x1010 (setFifoStart); bytes */
    uint32_t ring_shared_page_pfn;  /* from 0x101c write */
    uint32_t ring_base_pfn;         /* from 0x1030 (setFifoBasePage) */
    uint64_t ring_base_gpa;         /* (ring_base_pfn << 12) + ring_start_offset */
    uint32_t ring_size;             /* from 0x1004 (setFifoLength); default 64 KiB */
    uint32_t read_ptr;              /* decoder drain cursor (high-water mark) */
    uint32_t write_ptr;             /* last-seen guest write ptr (doorbell) */
    uint32_t last_completed_stamp;

    /* Pending stamp-completion bitmask, per A4d (2026-04-24).
     *
     * The kext's unified ISR at vector 0 reads three BAR0 status regs:
     *   0x1018 — stamp bitmask fed to AppleParavirtEventMachine::signalStamps
     *   0x1014 — display bitmask fed to AppleParavirtDisplayMachine::signalDisplays
     *   0x102c — fault-pending status (0 means no fault queued)
     *
     * Bit N of the 0x1018 mask indicates stamp_id N completed since the
     * last ISR. signalStamps loops set bits via `bsf`, calls commandWakeup
     * per stamp_id, which reads the actual stamp value from [EM+0x20]
     * in kernel heap (NOT from a DMA page). Our job is simply to signal
     * "which stamp IDs completed" via the bitmask and raise MSI-X.
     *
     * For the RootChannel single-sid case (the init path), stamp_id=0.
     * So completing a root-ring command sets bit 0.
     *
     * Xchg-and-clear semantics: guest ISR reads 0x1018 and atomically
     * clears the mask. We mirror that by returning the mask then zeroing
     * the field. */
    uint32_t pending_stamps_bitmask;

    /* Pending display-completion bitmask, read at BAR0+0x1014.
     *
     * Per display0-cmd-actual-location.md (2026-04-25): display channels
     * (Display0 = ch 5, Display1 = ch 6) signal completion via the
     * display bitmask, NOT the stamp bitmask. Setting bit ch in this
     * field tells the kext's display ISR (signalDisplays) to wake the
     * wrangler thread.
     *
     * Same xchg-and-clear semantics as 0x1018. */
    uint32_t pending_displays_bitmask;

    /* GPA of the current in-flight command's header on the ring.
     * Set by the drain immediately before calling dispatch_one so that
     * handlers can DMA-write into the on-ring header (e.g. 0x3a writes
     * actual_count to ring header +4 BEFORE stamp/IRQ fires — otherwise
     * the guest services the IRQ and reads the stale length). */
    uint64_t current_cmd_header_gpa;

    /* CmdGetDeviceInfo2 (opcode 0x3a) count-writeback state.
     * Used internally by the handler; see note above on the ordering
     * requirement. */
    uint32_t device_info_actual_count;

    /* Handle tables. */
    lagfx_task_entry_t      tasks[LAGFX_MAX_TASKS];
    lagfx_childfifo_entry_t fifos[LAGFX_MAX_CHILDFIFOS];
    lagfx_inflight_entry_t  inflight[LAGFX_MAX_INFLIGHT];
    lagfx_display_entry_t   displays[LAGFX_PROTO_MAX_DISPLAYS];

    /* Stats / observability. */
    uint64_t total_cmds_seen;
    uint64_t total_cmds_completed;
    uint64_t unknown_opcode_count;
    uint64_t interrupts_raised;

    /* Phase 2.A display counters (observable by tests). */
    uint64_t display_swaps_applied;
    uint64_t display_transactions_submitted;
    uint64_t display_acks_received;
};

/* Internal helper — index into reg[] by MMIO offset. Returns -1 if
 * the offset is not a recognized register. */
static inline int lagfx_protocol_reg_index(uint64_t offset) {
    if (offset < LAGFX_REG_BASE || offset > LAGFX_REG_LAST) {
        return -1;
    }
    if ((offset - LAGFX_REG_BASE) % 4u != 0u) {
        return -1;
    }
    return (int)((offset - LAGFX_REG_BASE) / 4u);
}

static inline bool lagfx_protocol_is_valid(const lagfx_protocol_t *p) {
    return p != NULL && p->magic == LAGFX_PROTOCOL_MAGIC;
}

/* Completion path — writes the host-to-guest stamp cell (readable at
 * MMIO 0x1014) and raises the IRQ. Every command completes
 * unconditionally; the 12-byte header has no flags field and the
 * dylib's handler tails always call the "signal stamp" selector
 * (re-followup-spec-gaps.md §5.1).
 *
 * Stamp-cell writes are monotonic — internally clamped to
 * max(target, cur+1) with a non-zero floor, so a stale `stamp` value
 * coming from the ring (often 0 in practice) cannot regress the cell
 * and park the kext (see library/howto/how-to-host-stamp-completion.md
 * + library/m3-prod-readiness-audit.md HIGH item 1). */
void lagfx_protocol_complete_stamp(lagfx_protocol_t *p, uint32_t stamp);

/* As above but for non-RootChannel completions (per-channel doorbells,
 * Display0/1/2, vchan completions). `slot` indexes into the FIFO base
 * page: cell GPA = (ring_base_pfn<<12) + slot*4. The pending-stamps
 * bitmask bit set is bit `slot` (matches signalStamps's bsf
 * iteration). */
void lagfx_protocol_complete_stamp_slot(lagfx_protocol_t *p,
                                        uint32_t slot,
                                        uint32_t stamp);

/* Per-channel variant of dispatch_one — runs the opcode handler but
 * does NOT auto-complete the stamp. Caller advances stamp_cell[ch] +
 * pending_stamps_bitmask + IRQ once after draining all cmds in the
 * ring. Returns the handler rc; the parsed cmd header is returned via
 * *out_hdr (non-NULL). Used by the per-channel doorbell handler at
 * BAR0+0x1020. */
int lagfx_protocol_dispatch_one_no_stamp(lagfx_protocol_t *p,
                                         const uint8_t *cmd_bytes,
                                         size_t cmd_len,
                                         lagfx_cmd_header_t *out_hdr);

/* Translate a task-virtual device address to a guest-physical address
 * via the per-task root-page PFN-array (published by CmdDefineHostTask
 * 0x38). On success returns true and writes the GPA + how many bytes
 * are contiguous before the next page boundary (so callers can chunk
 * reads). On miss (no registered host-task or zero page entry),
 * returns false. The caller can then choose to fall back to treating
 * the dev_addr as a literal GPA, or fail.
 *
 * Used by the CmdExecIndirect2 segment walker to read per-resource
 * cmdBufs whose host_gpu_addr is a task-virtual address rather than a
 * literal GPA. */
bool lagfx_task_translate(lagfx_protocol_t *p, uint32_t task_id,
                          uint64_t dev_addr, uint64_t *out_gpa,
                          uint64_t *out_run_len);

/* === Task / FIFO table helpers =================================
 *
 * Tiny linear scans — the tables are small (16 and 8 entries) and
 * Phase 1.A.2 is single-threaded. All helpers return NULL on miss
 * or if the protocol handle is invalid. */

static inline lagfx_task_entry_t *
lagfx_protocol_find_task(lagfx_protocol_t *p, uint32_t task_id) {
    if (!lagfx_protocol_is_valid(p)) {
        return NULL;
    }
    for (unsigned i = 0; i < LAGFX_MAX_TASKS; ++i) {
        if (p->tasks[i].live && p->tasks[i].id == task_id) {
            return &p->tasks[i];
        }
    }
    return NULL;
}

static inline lagfx_task_entry_t *
lagfx_protocol_alloc_task_slot(lagfx_protocol_t *p) {
    if (!lagfx_protocol_is_valid(p)) {
        return NULL;
    }
    for (unsigned i = 0; i < LAGFX_MAX_TASKS; ++i) {
        if (!p->tasks[i].live) {
            return &p->tasks[i];
        }
    }
    return NULL;
}

static inline lagfx_childfifo_entry_t *
lagfx_protocol_find_fifo(lagfx_protocol_t *p, uint32_t fifo_id) {
    if (!lagfx_protocol_is_valid(p)) {
        return NULL;
    }
    for (unsigned i = 0; i < LAGFX_MAX_CHILDFIFOS; ++i) {
        if (p->fifos[i].live && p->fifos[i].id == fifo_id) {
            return &p->fifos[i];
        }
    }
    return NULL;
}

static inline lagfx_childfifo_entry_t *
lagfx_protocol_alloc_fifo_slot(lagfx_protocol_t *p) {
    if (!lagfx_protocol_is_valid(p)) {
        return NULL;
    }
    for (unsigned i = 0; i < LAGFX_MAX_CHILDFIFOS; ++i) {
        if (!p->fifos[i].live) {
            return &p->fifos[i];
        }
    }
    return NULL;
}

static inline lagfx_display_entry_t *
lagfx_protocol_find_display(lagfx_protocol_t *p, uint32_t display_id) {
    if (!lagfx_protocol_is_valid(p)) {
        return NULL;
    }
    for (unsigned i = 0; i < LAGFX_PROTO_MAX_DISPLAYS; ++i) {
        if (p->displays[i].live && p->displays[i].id == display_id) {
            return &p->displays[i];
        }
    }
    return NULL;
}

static inline lagfx_display_entry_t *
lagfx_protocol_alloc_display_slot(lagfx_protocol_t *p) {
    if (!lagfx_protocol_is_valid(p)) {
        return NULL;
    }
    for (unsigned i = 0; i < LAGFX_PROTO_MAX_DISPLAYS; ++i) {
        if (!p->displays[i].live) {
            return &p->displays[i];
        }
    }
    return NULL;
}

/* Find-or-allocate: called from CmdDisplaySwapMapping and
 * CmdDisplayTransaction3 which both auto-register a previously-unseen
 * displayID rather than erroring. Matches dylib fail-open semantics
 * (command-buffer-format.md §6). */
static inline lagfx_display_entry_t *
lagfx_protocol_get_or_alloc_display(lagfx_protocol_t *p, uint32_t display_id) {
    lagfx_display_entry_t *d = lagfx_protocol_find_display(p, display_id);
    if (d) return d;
    d = lagfx_protocol_alloc_display_slot(p);
    if (d) {
        /* Initialise — live is set by the caller after populating. */
        *d = (lagfx_display_entry_t){0};
        d->id = display_id;
    }
    return d;
}

#endif /* LIBAPPLEGFX_PROTOCOL_STATE_H */

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

typedef struct {
    uint32_t      id;         /* taskID from CmdDefineTask2 */
    lagfx_task_t *shell_task; /* opaque handle from shell.create_task */
    uint64_t      base_va;    /* base of reserved VA range */
    uint64_t      length;
    bool          live;
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
} lagfx_display_entry_t;

struct lagfx_protocol {
    uint32_t magic;                 /* LAGFX_PROTOCOL_MAGIC */
    struct lagfx_device *dev;       /* back-pointer for shell callbacks */

    /* MMIO register shadow — 15 regs * 4 bytes, indexed by
     * (offset - LAGFX_REG_BASE) / 4. Covers 0x1000..0x1038. */
    uint32_t reg[LAGFX_REG_COUNT];

    /* Ring geometry (stubbed per R1 — ring_base_gpa is derived from
     * the three setter MMIO writes once the offset↔setter mapping is
     * nailed down by runtime capture). */
    bool     ring_armed;
    uint64_t ring_base_gpa;         /* TODO(R1): not populated yet */
    uint32_t ring_size;
    uint32_t read_ptr;              /* decoder drain cursor */
    uint32_t write_ptr;             /* last-seen guest write ptr (doorbell) */
    uint32_t last_completed_stamp;

    /* Setter-candidate probe state — see protocol.h:
     * the true doorbell offset in 0x1004..0x1034 is unknown, so we
     * log each write to the range and expose it for runtime capture. */
    uint32_t last_setter_offset;    /* last offset in the candidate range */
    uint32_t last_setter_value;     /* last value written there */
    uint64_t setter_write_count;

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
 * (re-followup-spec-gaps.md §5.1). */
void lagfx_protocol_complete_stamp(lagfx_protocol_t *p, uint32_t stamp);

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

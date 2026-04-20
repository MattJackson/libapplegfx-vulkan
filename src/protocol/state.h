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

/* Table capacities — see phase-1a2-decoder-plan.md §5. */
#define LAGFX_MAX_TASKS       16u
#define LAGFX_MAX_CHILDFIFOS   8u
#define LAGFX_MAX_INFLIGHT    32u

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
} lagfx_childfifo_entry_t;

typedef struct {
    uint32_t stamp;
    uint16_t opcode;
    bool     completed;
} lagfx_inflight_entry_t;

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

    /* Stats / observability. */
    uint64_t total_cmds_seen;
    uint64_t total_cmds_completed;
    uint64_t unknown_opcode_count;
    uint64_t interrupts_raised;
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

#endif /* LIBAPPLEGFX_PROTOCOL_STATE_H */

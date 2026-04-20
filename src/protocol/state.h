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
    uint8_t  opcode;
    uint8_t  flags;
    bool     completed;
} lagfx_inflight_entry_t;

struct lagfx_protocol {
    uint32_t magic;                 /* LAGFX_PROTOCOL_MAGIC */
    struct lagfx_device *dev;       /* back-pointer for shell callbacks */

    /* MMIO register shadow — 11 regs * 4 bytes, indexed by
     * (offset - LAGFX_REG_BASE) / 4. */
    uint32_t reg[LAGFX_REG_COUNT];

    /* Ring state (stubbed per R1). */
    bool     ring_armed;
    uint64_t ring_base_gpa;         /* TODO(R1): not populated yet */
    uint32_t ring_size;
    uint32_t read_ptr;              /* decoder drain cursor */
    uint32_t write_ptr;             /* last-seen guest write ptr */
    uint32_t last_doorbell_stamp;
    uint32_t last_completed_stamp;

    /* Handle tables. */
    lagfx_task_entry_t      tasks[LAGFX_MAX_TASKS];
    lagfx_childfifo_entry_t fifos[LAGFX_MAX_CHILDFIFOS];
    lagfx_inflight_entry_t  inflight[LAGFX_MAX_INFLIGHT];

    /* Stats / observability. */
    uint64_t total_cmds_seen;
    uint64_t total_cmds_completed;
    uint64_t unknown_opcode_count;
    uint64_t doorbell_writes;
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

/* Completion path — writes fence, optionally raises interrupt.
 * Defined in protocol.c; called by handlers + fifo.c. */
void lagfx_protocol_complete_stamp(lagfx_protocol_t *p,
                                   uint32_t stamp, uint8_t flags);

#endif /* LIBAPPLEGFX_PROTOCOL_STATE_H */

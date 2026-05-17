/*
 * libapplegfx-vulkan — Compute-segment inner-opcode dispatch (encType=0/1)
 * src/handlers/compute/compute_inner_ops.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * encType=0 (regular compute) and encType=1 (alt compute) inner
 * streams arrive via exec_cmdbuf.c::inner_walk_segment. This module
 * provides the dispatch table that routes individual inner opcodes
 * to per-opcode handlers.
 *
 * Stage 30 starting point (Task 6 of memory/stage30_freshman_queue.md):
 * the table is initially populated with parse-and-trace stubs for
 * the 15 encType=0 opcodes observed live (2026-05-14/15 empirical
 * sweep — 181k events, 100% encType=0, see paravirt-re/library/
 * journey/iteration-2-encType0-only-confirmed-2026-05-14.md). As
 * the freshman queue's Task 1 catalogs the wire format of each
 * opcode, individual handlers gain real parse logic. Task 6 promotes
 * at least ONE handler to a real vkCmdDispatch / vkCmdClearColorImage
 * / vkCmdFillBuffer translation that produces visible pixels.
 *
 * Private to src/handlers/compute/. Not installed.
 */

#ifndef LAGFX_HANDLERS_COMPUTE_COMPUTE_INNER_OPS_H
#define LAGFX_HANDLERS_COMPUTE_COMPUTE_INNER_OPS_H

#include <stddef.h>
#include <stdint.h>

typedef struct lagfx_protocol lagfx_protocol_t;

/* Dispatch one inner opcode from an encType=0 / encType=1 segment.
 *
 * Returns 0 on success, non-zero on unknown / malformed opcode.
 * Unknown opcodes are logged at LAGFX_TRACE so they don't drown
 * the steady-state log — the descriptor table covers the 15 known
 * encType=0 opcodes from the 2026-05-14 empirical sweep, and any
 * truly unknown opcode is worth surfacing in a follow-up cycle.
 *
 * `encoder_type` is 0 or 1 (the walker filters out non-compute
 * encTypes before reaching this dispatcher). Today the body parsers
 * are encoder-type-agnostic; future versions may diverge.
 */
int lagfx_compute_inner_dispatch(lagfx_protocol_t *p,
                                  uint32_t          encoder_type,
                                  uint32_t          task_id,
                                  uint32_t          opcode,
                                  const uint8_t    *body,
                                  size_t            body_len);

/* Human-readable name for an inner opcode. Returns "unknown" if not
 * in the table. Useful for log enrichment. */
const char *lagfx_compute_inner_op_name(uint32_t opcode);

#endif /* LAGFX_HANDLERS_COMPUTE_COMPUTE_INNER_OPS_H */

/*
 * libapplegfx-vulkan — Compute inner-opcode handlers + dispatch
 * src/handlers/compute/compute_inner_ops.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Architecture: encType=0 / encType=1 segments arrive via
 * exec_cmdbuf.c::inner_walk_segment. This file is the dispatch
 * table for individual inner opcodes within those segments.
 *
 * Initial population (2026-05-16 senior, Task 6 prep): the 15
 * encType=0 opcodes observed live on 2026-05-14 (181k inner_walk
 * events, all encType=0). Each is currently a parse-and-trace stub
 * with TODO: Stage 30 markers.
 *
 * As the freshman queue's Task 1 produces the opcode catalog
 * (`paravirt-re/library/journey/enctype0-opcode-catalog-*.md`),
 * individual stubs gain real body parsing. Task 6 promotes ONE
 * stub to a real Vulkan translation (vkCmdClearColorImage /
 * vkCmdFillBuffer / vkCmdDispatch) that produces visible pixels.
 *
 * Observed encType=0 opcode frequency (current container, ~22 min):
 *   0x007e  27475   most-common; semantic UNKNOWN — Task 1
 *   0x0074  26664   semantic UNKNOWN — Task 1
 *   0x007d  23347   semantic UNKNOWN — Task 1
 *   0x0007  22439   semantic UNKNOWN — Task 1
 *   0x006e  15744   semantic UNKNOWN — Task 1
 *   0x0072  14890   semantic UNKNOWN — Task 1
 *   0x0075  12853   semantic UNKNOWN — Task 1
 *   0x0082   9577   semantic UNKNOWN — Task 1
 *   0x0070   8317   semantic UNKNOWN — Task 1
 *   0x001a   8168   NOT the render 0x1a RenderPassDescriptor —
 *                   encType=0 namespace; semantic UNKNOWN — Task 1
 *   0x0017   7416   semantic UNKNOWN — Task 1
 *   0x0003   4256   semantic UNKNOWN — Task 1
 *   0x0006    310   semantic UNKNOWN — Task 1
 *   0x006f    305   semantic UNKNOWN — Task 1
 *   0x0001     86   semantic UNKNOWN — Task 1
 *
 * Note: inner opcodes are NAMESPACED BY encType (CLAUDE.md
 * "Don't-do-this list" #21). encType=0 0x1a is NOT the render
 * 0x1a RenderPassDescriptor. Do not import semantics across
 * namespaces.
 */

#include "compute_inner_ops.h"

#include "common/log.h"

#include <stddef.h>
#include <stdint.h>

typedef int (*lagfx_compute_inner_op_fn)(lagfx_protocol_t *p,
                                          uint32_t          encoder_type,
                                          const uint8_t    *body,
                                          size_t            body_len);

typedef struct {
    uint32_t                    opcode;
    const char                 *name;
    lagfx_compute_inner_op_fn   handler;
} lagfx_compute_inner_op_desc_t;

/* === Generic parse-and-trace stub ============================== *
 *
 * Every encType=0 opcode currently routes here. The handler logs
 * the opcode + body length at LAGFX_TRACE. As Task 1 identifies
 * specific opcodes' wire formats, individual stubs replace this
 * generic one with a real parser.
 *
 * Returns 0 (success) unconditionally — observation only. */
static int op_trace_stub(lagfx_protocol_t *p,
                          uint32_t          encoder_type,
                          const uint8_t    *body,
                          size_t            body_len) {
    (void)p;
    LAGFX_TRACE("compute_inner: encType=%u op=??? body_len=%zu (stub)",
                (unsigned)encoder_type, body_len);
    (void)body;
    return 0;
}

/* === Opcode descriptor table ===================================== *
 *
 * Populated from the 2026-05-14 empirical sweep. Names are TBD
 * pending Task 1 catalog. Currently all entries route to the
 * generic trace stub.
 *
 * Sorted by descending observed frequency to favour log-cache
 * locality if we ever sort/binary-search this table. Linear
 * scan is fine for 15 entries. */
static const lagfx_compute_inner_op_desc_t compute_inner_op_table[] = {
    { 0x007e, "compute_op_7e_unknown",   op_trace_stub },
    { 0x0074, "compute_op_74_unknown",   op_trace_stub },
    { 0x007d, "compute_op_7d_unknown",   op_trace_stub },
    { 0x0007, "compute_op_07_unknown",   op_trace_stub },
    { 0x006e, "compute_op_6e_unknown",   op_trace_stub },
    { 0x0072, "compute_op_72_unknown",   op_trace_stub },
    { 0x0075, "compute_op_75_unknown",   op_trace_stub },
    { 0x0082, "compute_op_82_unknown",   op_trace_stub },
    { 0x0070, "compute_op_70_unknown",   op_trace_stub },
    { 0x001a, "compute_op_1a_unknown",   op_trace_stub },
    { 0x0017, "compute_op_17_unknown",   op_trace_stub },
    { 0x0003, "compute_op_03_unknown",   op_trace_stub },
    { 0x0006, "compute_op_06_unknown",   op_trace_stub },
    { 0x006f, "compute_op_6f_unknown",   op_trace_stub },
    { 0x0001, "compute_op_01_unknown",   op_trace_stub },
};

#define LAGFX_COMPUTE_INNER_OP_COUNT \
    (sizeof(compute_inner_op_table) / sizeof(compute_inner_op_table[0]))

static const lagfx_compute_inner_op_desc_t *
find_compute_inner_op_desc(uint32_t opcode) {
    for (size_t i = 0; i < LAGFX_COMPUTE_INNER_OP_COUNT; ++i) {
        if (compute_inner_op_table[i].opcode == opcode) {
            return &compute_inner_op_table[i];
        }
    }
    return NULL;
}

int lagfx_compute_inner_dispatch(lagfx_protocol_t *p,
                                  uint32_t          encoder_type,
                                  uint32_t          opcode,
                                  const uint8_t    *body,
                                  size_t            body_len) {
    const lagfx_compute_inner_op_desc_t *desc =
        find_compute_inner_op_desc(opcode);
    if (!desc) {
        LAGFX_TRACE("compute_inner: encType=%u op=0x%04x body_len=%zu "
                    "(UNKNOWN — not in encType=0 table)",
                    (unsigned)encoder_type, (unsigned)opcode, body_len);
        return 1;
    }
    return desc->handler(p, encoder_type, body, body_len);
}

const char *lagfx_compute_inner_op_name(uint32_t opcode) {
    const lagfx_compute_inner_op_desc_t *desc =
        find_compute_inner_op_desc(opcode);
    return desc ? desc->name : "unknown";
}

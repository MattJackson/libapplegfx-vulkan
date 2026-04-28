/*
 * libapplegfx-vulkan — Compute-decoder dispatch table (M5 scaffold)
 * src/protocol/compute_opcodes.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Populates the 32-entry descriptor table for the Compute inner-opcode
 * decoder. All entries point at `compute_op_ack_stub`; real handlers
 * land one-by-one as M5 progresses. Entries are listed in numerical
 * opcode order (matches the row order of
 * paravirt-re/library/state-machines/compute-decoder-handlers.tsv).
 *
 * Body sizes come from the TSV's struct_size_bytes column. Variable-
 * length entries (payload of the form "header + N*entry_size") store 0
 * in body_size — handlers that consume them parse the count themselves;
 * the real length arrives via the wire-level totalLengthBytes field.
 *
 * Opcodes 0x86 (UseHeaps) and 0x87 (UseResources) are shared with the
 * Render decoder. When encoderType==1 they dispatch through this table;
 * when encoderType==2 they dispatch through the render opcode table.
 * Opcode 0xdb (SetComputePassDispatchType) is descriptor-only — it is
 * only valid inside decodeComputePassDescriptor: and never reaches the
 * main dispatch loop, so it is excluded from this table.
 */

#include "compute_opcodes.h"

#include "../common/log.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int compute_op_ack_stub(lagfx_protocol_t *p,
                               const uint8_t    *payload,
                               size_t            len) {
    (void)p;
    (void)payload;
    (void)len;
    return 0;
}

bool lagfx_compute_op_is_stub(uint32_t opcode) {
    const lagfx_compute_op_descriptor_t *d = lagfx_compute_op_lookup(opcode);
    return d != NULL && d->default_handler == compute_op_ack_stub;
}

static const lagfx_compute_op_descriptor_t g_compute_op_table[] = {
    /* Shared with Render decoder (valid when encoderType==1). */
    { 0x86, "UseHeaps",                                   0, 0, compute_op_ack_stub },
    { 0x87, "UseResources",                               0, 0, compute_op_ack_stub },

    /* Main compute opcode range 0xc8..0xe6 (minus 0xdb). */
    { 0xc8, "DispatchThreadgroups",                      48, 0, compute_op_ack_stub },
    { 0xc9, "DispatchThreadgroupsIndirect",              36, 1, compute_op_ack_stub },
    { 0xca, "DispatchThreads",                           48, 0, compute_op_ack_stub },
    /* 0xcb SetBuffers — variable "8+N*12". */
    { 0xcb, "SetBuffers",                                 0, 0, compute_op_ack_stub },
    /* 0xcc SetSamplers — variable "8+N*4". */
    { 0xcc, "SetSamplers",                                0, 0, compute_op_ack_stub },
    /* 0xcd SetSamplersLODClamp — variable "8+N*12". */
    { 0xcd, "SetSamplersLODClamp",                        0, 0, compute_op_ack_stub },
    /* 0xce SetTextures — variable "8+N*4". */
    { 0xce, "SetTextures",                                0, 0, compute_op_ack_stub },
    { 0xcf, "SetBufferOffset",                           12, 0, compute_op_ack_stub },
    { 0xd0, "SetPipelineState",                           4, 1, compute_op_ack_stub },
    { 0xd1, "SetStageInRegion",                          48, 0, compute_op_ack_stub },
    { 0xd2, "SetStageInRegionIndirect",                  12, 1, compute_op_ack_stub },
    { 0xd3, "SetThreadgroupMemoryLength",                12, 0, compute_op_ack_stub },
    { 0xd4, "UpdateFence",                                4, 1, compute_op_ack_stub },
    { 0xd5, "WaitForFence",                               4, 1, compute_op_ack_stub },
    /* 0xd6 BarrierResources — variable "4+N*4". */
    { 0xd6, "BarrierResources",                           0, 0, compute_op_ack_stub },
    { 0xd7, "BarrierScope",                               4, 0, compute_op_ack_stub },
    { 0xd8, "SetImageblockWidth",                         8, 0, compute_op_ack_stub },
    /* 0xd9 SetBuffersWithStride — variable "8+N*20". */
    { 0xd9, "SetBuffersWithStride",                       0, 0, compute_op_ack_stub },
    { 0xda, "SetBufferOffsetWithStride",                 20, 0, compute_op_ack_stub },

    /* 0xdb is descriptor-only (SetComputePassDispatchType) — excluded. */

    /* Control-flow opcodes (require MTLCommandBufferJump). */
    { 0xdc, "EncodeStartDoWhile",                         0, 0, compute_op_ack_stub },
    { 0xdd, "EncodeEndDoWhile",                          20, 1, compute_op_ack_stub },
    { 0xde, "EncodeStartWhile",                          20, 1, compute_op_ack_stub },
    { 0xdf, "EncodeEndWhile",                             0, 0, compute_op_ack_stub },
    { 0xe0, "EncodeStartIf",                             20, 1, compute_op_ack_stub },
    { 0xe1, "EncodeStartElse",                            0, 0, compute_op_ack_stub },
    { 0xe2, "EncodeEndIf",                                0, 0, compute_op_ack_stub },
    { 0xe3, "InsertCompressedTextureReinterpretationFlush", 0, 0, compute_op_ack_stub },
    { 0xe4, "ExecuteCommandInBufferRanged",              20, 1, compute_op_ack_stub },
    { 0xe5, "ExecuteCommandInBuffer",                    16, 2, compute_op_ack_stub },
    { 0xe6, "DispatchThreadsIndirect",                   12, 1, compute_op_ack_stub },
};

static const size_t g_compute_op_table_count =
    sizeof(g_compute_op_table) / sizeof(g_compute_op_table[0]);

_Static_assert(sizeof(g_compute_op_table) / sizeof(g_compute_op_table[0]) ==
               LAGFX_COMPUTE_OPCODE_COUNT,
               "compute_opcodes.c table size must match "
               "LAGFX_COMPUTE_OPCODE_COUNT (32)");

const lagfx_compute_op_descriptor_t *
lagfx_compute_op_lookup(uint32_t opcode) {
    if (opcode < LAGFX_COMPUTE_OPCODE_MIN || opcode > LAGFX_COMPUTE_OPCODE_MAX) {
        return NULL;
    }
    for (size_t i = 0; i < g_compute_op_table_count; ++i) {
        if (g_compute_op_table[i].opcode == opcode) {
            return &g_compute_op_table[i];
        }
    }
    return NULL;
}

size_t lagfx_compute_op_table_size(void) {
    return g_compute_op_table_count;
}

const lagfx_compute_op_descriptor_t *
lagfx_compute_op_table_entry(size_t index) {
    if (index >= g_compute_op_table_count) {
        return NULL;
    }
    return &g_compute_op_table[index];
}

const char *lagfx_compute_op_name(uint32_t opcode) {
    const lagfx_compute_op_descriptor_t *d = lagfx_compute_op_lookup(opcode);
    if (d) {
        return d->name;
    }
    static char unknown_buf[28];
    snprintf(unknown_buf, sizeof(unknown_buf), "Unknown(0x%03x)",
             (unsigned)(opcode & 0xffffu));
    return unknown_buf;
}

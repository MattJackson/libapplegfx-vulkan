/*
 * libapplegfx-vulkan — Compute-decoder inner-opcode enum + descriptor (M5)
 * src/protocol/compute_opcodes.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The Compute decoder (PGDeserializerComputeDecoder, encoderType=1 in the
 * inner-opcode wire format — see
 * paravirt-re/library/state-machines/inner-opcode-format.md) recognises
 * 32 inner opcodes: 30 in the contiguous range 0xc8..0xe6 (minus 0xdb
 * which is descriptor-only) plus 2 shared opcodes 0x86 (UseHeaps) and
 * 0x87 (UseResources) that are also valid in the Render decoder when
 * encoderType=2. Each enumerated value carries:
 *
 *   - the on-wire opcode (low u32 of the 8-byte PGCmdHeader),
 *   - a fixed body size (or 0 if variable-length — headcount-prefixed
 *     lists store the element count in the payload itself),
 *   - a "reference count" — the number of resource references the
 *     decoder retains for this command (variable-length entries store 0;
 *     the real count is runtime from the per-entry array).
 *
 *   paravirt-re/library/state-machines/compute-decoder-handlers.tsv
 *
 * M5 scaffold (this file): every opcode resolves to an ack-only stub
 * (logs and returns OK). When real handlers land, the `default_handler`
 * field on each descriptor will be replaced one-by-one with the real
 * implementation.
 *
 * This header is private to src/protocol/. Not installed.
 */

#ifndef LIBAPPLEGFX_PROTOCOL_COMPUTE_OPCODES_H
#define LIBAPPLEGFX_PROTOCOL_COMPUTE_OPCODES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct lagfx_protocol lagfx_protocol_t;

typedef enum {
    LAGFX_COMPUTE_OP_USE_HEAPS                                 = 0x86,
    LAGFX_COMPUTE_OP_USE_RESOURCES                             = 0x87,
    LAGFX_COMPUTE_OP_DISPATCH_THREADGROUPS                     = 0xc8,
    LAGFX_COMPUTE_OP_DISPATCH_THREADGROUPS_INDIRECT            = 0xc9,
    LAGFX_COMPUTE_OP_DISPATCH_THREADS                          = 0xca,
    LAGFX_COMPUTE_OP_SET_BUFFERS                               = 0xcb,
    LAGFX_COMPUTE_OP_SET_SAMPLERS                              = 0xcc,
    LAGFX_COMPUTE_OP_SET_SAMPLERS_LOD_CLAMP                    = 0xcd,
    LAGFX_COMPUTE_OP_SET_TEXTURES                              = 0xce,
    LAGFX_COMPUTE_OP_SET_BUFFER_OFFSET                         = 0xcf,
    LAGFX_COMPUTE_OP_SET_PIPELINE_STATE                        = 0xd0,
    LAGFX_COMPUTE_OP_SET_STAGE_IN_REGION                       = 0xd1,
    LAGFX_COMPUTE_OP_SET_STAGE_IN_REGION_INDIRECT              = 0xd2,
    LAGFX_COMPUTE_OP_SET_THREADGROUP_MEMORY_LENGTH             = 0xd3,
    LAGFX_COMPUTE_OP_UPDATE_FENCE                              = 0xd4,
    LAGFX_COMPUTE_OP_WAIT_FOR_FENCE                            = 0xd5,
    LAGFX_COMPUTE_OP_BARRIER_RESOURCES                         = 0xd6,
    LAGFX_COMPUTE_OP_BARRIER_SCOPE                             = 0xd7,
    LAGFX_COMPUTE_OP_SET_IMAGEBLOCK_WIDTH                      = 0xd8,
    LAGFX_COMPUTE_OP_SET_BUFFERS_WITH_STRIDE                   = 0xd9,
    LAGFX_COMPUTE_OP_SET_BUFFER_OFFSET_WITH_STRIDE             = 0xda,
    LAGFX_COMPUTE_OP_ENCODE_START_DO_WHILE                     = 0xdc,
    LAGFX_COMPUTE_OP_ENCODE_END_DO_WHILE                       = 0xdd,
    LAGFX_COMPUTE_OP_ENCODE_START_WHILE                        = 0xde,
    LAGFX_COMPUTE_OP_ENCODE_END_WHILE                          = 0xdf,
    LAGFX_COMPUTE_OP_ENCODE_START_IF                           = 0xe0,
    LAGFX_COMPUTE_OP_ENCODE_START_ELSE                         = 0xe1,
    LAGFX_COMPUTE_OP_ENCODE_END_IF                             = 0xe2,
    LAGFX_COMPUTE_OP_INSERT_COMPRESSED_TEXTURE_REINTERP_FLUSH  = 0xe3,
    LAGFX_COMPUTE_OP_EXECUTE_COMMAND_IN_BUFFER_RANGED          = 0xe4,
    LAGFX_COMPUTE_OP_EXECUTE_COMMAND_IN_BUFFER                 = 0xe5,
    LAGFX_COMPUTE_OP_DISPATCH_THREADS_INDIRECT                 = 0xe6,
} lagfx_compute_op_t;

#define LAGFX_COMPUTE_OPCODE_COUNT  32u
#define LAGFX_COMPUTE_OPCODE_MIN    0x86u
#define LAGFX_COMPUTE_OPCODE_MAX    0xe6u

typedef int (*lagfx_compute_op_handler_fn)(lagfx_protocol_t *p,
                                           const uint8_t   *payload,
                                           size_t           len);

typedef struct {
    uint32_t                    opcode;
    const char                 *name;
    uint32_t                    body_size;
    uint32_t                    ref_count;
    lagfx_compute_op_handler_fn default_handler;
} lagfx_compute_op_descriptor_t;

const lagfx_compute_op_descriptor_t *
lagfx_compute_op_lookup(uint32_t opcode);

size_t lagfx_compute_op_table_size(void);
const lagfx_compute_op_descriptor_t *
lagfx_compute_op_table_entry(size_t index);

const char *lagfx_compute_op_name(uint32_t opcode);

bool lagfx_compute_op_is_stub(uint32_t opcode);

#endif /* LIBAPPLEGFX_PROTOCOL_COMPUTE_OPCODES_H */

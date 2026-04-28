/*
 * libapplegfx-vulkan — Blit-decoder inner-opcode enum + descriptor (M5)
 * src/protocol/blit_opcodes.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The Blit decoder (PGDeserializerBlitDecoder, encoderType=0 in the
 * inner-opcode wire format — see
 * paravirt-re/library/state-machines/inner-opcode-format.md) recognises
 * 24 inner opcodes in the contiguous range 0x12c..0x143. Each
 * enumerated value carries:
 *
 *   - the on-wire opcode (low u32 of the 8-byte PGCmdHeader),
 *   - a fixed body size (all blit opcodes are fixed-length — no
 *     variable-length entries unlike the Render decoder),
 *   - a "reference count" — the number of resource references the
 *     decoder retains for this command (used by the residency tracker
 *     once the real handlers land).
 *
 *   paravirt-re/library/state-machines/blit-decoder-handlers.tsv
 *
 * M5 scaffold (this file): every opcode resolves to an ack-only stub
 * (logs and returns OK). When real handlers land, the `default_handler`
 * field on each descriptor will be replaced one-by-one with the real
 * implementation.
 *
 * This header is private to src/protocol/. Not installed.
 */

#ifndef LIBAPPLEGFX_PROTOCOL_BLIT_OPCODES_H
#define LIBAPPLEGFX_PROTOCOL_BLIT_OPCODES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct lagfx_protocol lagfx_protocol_t;

typedef enum {
    LAGFX_BLIT_OP_COPY_FROM_BUFFER_TO_TEXTURE              = 0x12c,
    LAGFX_BLIT_OP_COPY_FROM_BUFFER_TO_BUFFER               = 0x12d,
    LAGFX_BLIT_OP_COPY_FROM_TEXTURE_TO_BUFFER              = 0x12e,
    LAGFX_BLIT_OP_COPY_FROM_TEXTURE_TO_TEXTURE             = 0x12f,
    LAGFX_BLIT_OP_COPY_FROM_TEXTURE_TO_TEXTURE_WITH_OPTIONS = 0x130,
    LAGFX_BLIT_OP_COPY_INDIRECT_COMMAND_BUFFER             = 0x131,
    LAGFX_BLIT_OP_FILL_BUFFER                              = 0x132,
    LAGFX_BLIT_OP_GENERATE_MIPMAPS                         = 0x133,
    LAGFX_BLIT_OP_OPTIMIZE_FOR_CPU_ACCESS                  = 0x134,
    LAGFX_BLIT_OP_OPTIMIZE_FOR_GPU_ACCESS                  = 0x135,
    LAGFX_BLIT_OP_OPTIMIZE_IMAGE_FOR_CPU_ACCESS            = 0x136,
    LAGFX_BLIT_OP_OPTIMIZE_IMAGE_FOR_GPU_ACCESS            = 0x137,
    LAGFX_BLIT_OP_OPTIMIZE_INDIRECT_COMMAND_BUFFER         = 0x138,
    LAGFX_BLIT_OP_RESET_COMMANDS_IN_COMMAND_BUFFER         = 0x139,
    LAGFX_BLIT_OP_SYNCHRONIZE_RESOURCE                     = 0x13a,
    LAGFX_BLIT_OP_SYNCHRONIZE_TEXTURE_IMAGE                = 0x13b,
    LAGFX_BLIT_OP_BLIT_UPDATE_FENCE                        = 0x13c,
    LAGFX_BLIT_OP_BLIT_WAIT_FOR_FENCE                      = 0x13d,
    LAGFX_BLIT_OP_COPY_FROM_TEXTURE_TO_TEXTURE_NUM_SLICE   = 0x13e,
    LAGFX_BLIT_OP_FILL_BUFFER_WITH_PATTERN                 = 0x13f,
    LAGFX_BLIT_OP_FILL_TEXTURE_WITH_BYTES                  = 0x140,
    LAGFX_BLIT_OP_FILL_TEXTURE_WITH_COLOR                  = 0x141,
    LAGFX_BLIT_OP_INVALIDATE_COMPRESSED_TEXTURE            = 0x142,
    LAGFX_BLIT_OP_INVALIDATE_COMPRESSED_TEXTURE_IMAGE      = 0x143,
} lagfx_blit_op_t;

#define LAGFX_BLIT_OPCODE_COUNT  24u
#define LAGFX_BLIT_OPCODE_MIN    0x12cu
#define LAGFX_BLIT_OPCODE_MAX    0x143u

typedef int (*lagfx_blit_op_handler_fn)(lagfx_protocol_t *p,
                                        const uint8_t   *payload,
                                        size_t           len);

typedef struct {
    uint32_t                 opcode;
    const char              *name;
    uint32_t                 body_size;
    uint32_t                 ref_count;
    lagfx_blit_op_handler_fn default_handler;
} lagfx_blit_op_descriptor_t;

const lagfx_blit_op_descriptor_t *
lagfx_blit_op_lookup(uint32_t opcode);

size_t lagfx_blit_op_table_size(void);
const lagfx_blit_op_descriptor_t *
lagfx_blit_op_table_entry(size_t index);

const char *lagfx_blit_op_name(uint32_t opcode);

bool lagfx_blit_op_is_stub(uint32_t opcode);

#endif /* LIBAPPLEGFX_PROTOCOL_BLIT_OPCODES_H */

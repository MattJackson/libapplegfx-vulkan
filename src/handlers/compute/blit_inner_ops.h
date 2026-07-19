/*
 * libapplegfx-vulkan — Blit-segment inner-opcode dispatch (encType=4 blit)
 * src/handlers/compute/blit_inner_ops.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * The Blit decoder (PGDeserializerBlitDecoder, encType=4 in the
 * inner-opcode wire format) recognises 24 opcodes in the contiguous
 * range 0x12c..0x143 (RE: paravirt-re/library/state-machines/
 * blit-decoder-handlers.tsv) plus an extended low-range
 * (0x001..0x07d) of shared opcodes that occasionally appear in blit
 * encoder streams (RE: opcodes-encType4-extended.md).
 *
 * encType=4 is OVERLOADED in the wire format — opcodes 0x1c2..0x1d0
 * carry InfoDecoder reply queries (info_replies.c). The walker in
 * exec_cmdbuf.c routes those before reaching this table; this table
 * only sees real blit traffic.
 *
 * Private to src/handlers/compute/. Not installed.
 */

#ifndef LAGFX_HANDLERS_COMPUTE_BLIT_INNER_OPS_H
#define LAGFX_HANDLERS_COMPUTE_BLIT_INNER_OPS_H

#include <stddef.h>
#include <stdint.h>

typedef struct lagfx_protocol lagfx_protocol_t;

int lagfx_blit_inner_dispatch(lagfx_protocol_t *p,
                              uint32_t          opcode,
                              const uint8_t    *payload,
                              size_t            len);

const char *lagfx_blit_inner_op_name(uint32_t opcode);

#endif /* LAGFX_HANDLERS_COMPUTE_BLIT_INNER_OPS_H */

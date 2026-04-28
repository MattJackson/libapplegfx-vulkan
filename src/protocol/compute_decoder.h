/*
 * libapplegfx-vulkan — Compute-decoder dispatch entry point (M5 scaffold)
 * src/protocol/compute_decoder.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Single entry point for dispatching one Compute inner opcode. The
 * dispatcher looks the opcode up in the descriptor table populated by
 * `compute_opcodes.c` and runs the descriptor's `default_handler`
 * (currently always the ack-only stub). Unknown opcodes are logged and
 * absorbed (return 0) so the caller's segment walker can continue.
 *
 * This header is private to src/protocol/. Not installed.
 */

#ifndef LIBAPPLEGFX_PROTOCOL_COMPUTE_DECODER_H
#define LIBAPPLEGFX_PROTOCOL_COMPUTE_DECODER_H

#include <stddef.h>
#include <stdint.h>

typedef struct lagfx_protocol lagfx_protocol_t;

int lagfx_compute_decoder_dispatch(lagfx_protocol_t *p,
                                   uint32_t          opcode,
                                   const uint8_t    *payload,
                                   size_t            len);

#endif /* LIBAPPLEGFX_PROTOCOL_COMPUTE_DECODER_H */

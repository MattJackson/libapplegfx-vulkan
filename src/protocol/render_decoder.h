/*
 * libapplegfx-vulkan — Render-decoder dispatch entry point (M5 scaffold)
 * src/protocol/render_decoder.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Single entry point for dispatching one Render inner opcode. The
 * dispatcher looks the opcode up in the descriptor table populated by
 * `render_opcodes.c` and runs the descriptor's `default_handler`
 * (currently always the ack-only stub). Unknown opcodes are logged and
 * absorbed (return 0) so the caller's segment walker can continue —
 * matches the dylib's fail-open semantics on unknown commands.
 *
 * Wire-up note (M5 brief): this entry point is NOT yet hooked into
 * `ops_cmdbuf.c`'s segment walker. It stands alone as a self-contained
 * module that tests can drive directly. M5 will hook it in once M4
 * closes.
 *
 * This header is private to src/protocol/. Not installed.
 */

#ifndef LIBAPPLEGFX_PROTOCOL_RENDER_DECODER_H
#define LIBAPPLEGFX_PROTOCOL_RENDER_DECODER_H

#include <stddef.h>
#include <stdint.h>

/* Forward declaration. Full layout in state.h. */
typedef struct lagfx_protocol lagfx_protocol_t;

/* Dispatch a single Render inner opcode.
 *
 *   p       — owning protocol state (may be NULL for tests that only
 *             want to exercise the lookup).
 *   opcode  — on-wire u32 opcode field of the inner PGCmdHeader.
 *   payload — pointer to the `payloadLen` bytes that follow the
 *             8-byte PGCmdHeader (i.e. cursor sub-buffer body); may
 *             be NULL only if `len == 0`.
 *   len     — payload length in bytes (= totalLengthBytes - 8).
 *
 * Returns 0 on success (handler ran or unknown-opcode absorbed) and
 * non-zero on a hard error (e.g. handler reported malformed payload).
 * M5 scaffold: every known opcode resolves to the ack-only stub which
 * returns 0; unknown opcodes also return 0 after logging. */
int lagfx_render_decoder_dispatch(lagfx_protocol_t *p,
                                  uint32_t          opcode,
                                  const uint8_t    *payload,
                                  size_t            len);

#endif /* LIBAPPLEGFX_PROTOCOL_RENDER_DECODER_H */

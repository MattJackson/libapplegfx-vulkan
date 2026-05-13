/*
 * libapplegfx-vulkan — Render-segment inner-opcode dispatch (encType=2)
 * src/handlers/compute/render_inner_ops.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The Render decoder (PGDeserializerRenderDecoder, encType=2 in the
 * inner-opcode wire format — see paravirt-re/library/state-machines/
 * inner-opcode-format.md) recognises 96 inner opcodes:
 *
 *   0x00..0x1d  — Draw / barrier / fence / 0x1a RenderDescribeRenderPass
 *   0x2c        — Unknown (macOS sends len=88, RE pending)
 *   0x3c        — command-buffer-inner (recursive cmdbuf reference)
 *   0x65..0xa6  — State-set family (viewport, scissor, blend, vertex/
 *                 fragment/tile texture/buffer/sampler binds, etc.)
 *
 * Authoritative table source:
 *   paravirt-re/library/state-machines/render-decoder-handlers.tsv
 *
 * Live wire-up: inner_walk_segment in exec_cmdbuf.c calls
 * lagfx_render_inner_dispatch on encoder_type==2. Every opcode has an
 * explicit descriptor; unknown opcodes log + ack.
 *
 * Implementation status (2026-05-13 fresh-analysis port from the
 * pre-refactor render_opcodes.c at b652199~1, stranded in
 * dead-code-to-revive/ between 2026-05-12 and this commit):
 *   - Every handler parses its payload (bounded read + LAGFX_TRACE).
 *   - No Vulkan / render-encoder / resource-registry side-effects:
 *     those depended on lagfx_protocol_t fields (render_enc,
 *     current_task_id, dev->vk) that no longer exist in the post-
 *     dispatcher state.h.
 *   - When SkyLight starts submitting encType=2 segments, each handler
 *     will TRACE-log the parsed body so the next debugging pass has
 *     real wire-format data to reason from.
 *
 * Private to src/handlers/compute/. Not installed.
 */

#ifndef LAGFX_HANDLERS_COMPUTE_RENDER_INNER_OPS_H
#define LAGFX_HANDLERS_COMPUTE_RENDER_INNER_OPS_H

#include <stddef.h>
#include <stdint.h>

typedef struct lagfx_protocol lagfx_protocol_t;

/* Dispatch a single Render inner opcode. Returns 0 on success.
 *
 *   p       — owning protocol state.
 *   opcode  — low 16 bits of the inner PGCmdHeader opcode field.
 *   payload — body bytes following the 8-byte PGCmdHeader.
 *   len     — payload length (totalLengthBytes - 8). May be 0. */
int lagfx_render_inner_dispatch(lagfx_protocol_t *p,
                                uint32_t          opcode,
                                const uint8_t    *payload,
                                size_t            len);

/* Short human-readable name for tracing. Returns "Unknown(0xNN)"
 * for opcodes not in the table. Uses a static buffer (decoder is
 * single-threaded). */
const char *lagfx_render_inner_op_name(uint32_t opcode);

#endif /* LAGFX_HANDLERS_COMPUTE_RENDER_INNER_OPS_H */

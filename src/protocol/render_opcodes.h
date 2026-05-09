/*
 * libapplegfx-vulkan — Render-decoder inner-opcode enum + descriptor (M5)
 * src/protocol/render_opcodes.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The Render decoder (PGDeserializerRenderDecoder, encoderType=2 in the
 * inner-opcode wire format — see
 * paravirt-re/library/state-machines/inner-opcode-format.md) recognises
 * 96 inner opcodes in the range 0x00..0xa6 (sparse) plus the sub-decoder
 * opcode 0x1a (RenderDescribeRenderPass). Each enumerated
 * value carries:
 *
 *   - the on-wire opcode (low u32 of the 8-byte PGCmdHeader),
 *   - a fixed body size (or 0 if the body is variable-length;
 *     headcount-prefixed lists store the array element count in the
 *     `ref_count` slot per the TSV's "ref_count" column),
 *   - a "reference count" — the number of resource references the
 *     decoder retains for this command (used by the residency tracker
 *     once the real handlers land).
 *
 *   paravirt-re/library/state-machines/render-decoder-handlers.tsv
 * plus sub-decoder opcode 0x1a (RenderDescribeRenderPass).
 *
 * M5 scaffold (this file): every opcode resolves to an ack-only stub
 * (logs and returns OK). When M4 closes and real handlers land, the
 * `default_handler` field on each descriptor will be replaced one-by-one
 * with the real implementation. The dispatcher does NOT yet feed off
 * `ops_cmdbuf.c`'s segment walker — it stands alone behind
 * `lagfx_render_decoder_dispatch()` so tests / future segment-walk code
 * can drive it directly.
 *
 * This header is private to src/protocol/. Not installed.
 */

#ifndef LIBAPPLEGFX_PROTOCOL_RENDER_OPCODES_H
#define LIBAPPLEGFX_PROTOCOL_RENDER_OPCODES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "render_pass.h"

/* Forward declaration — full layout in state.h, but this scaffold only
 * needs the typedef so handler signatures are well-formed. */
typedef struct lagfx_protocol lagfx_protocol_t;

/* === Render inner-opcode enum (95 entries) =====================
 *
 * Numeric values match the on-wire u32 opcode field of PGCmdHeader.
 * The enum is sparse (gaps at 0x1a, 0x1e..0x64, 0x65..0xa6 dense).
 * Names mirror the Apple ObjC selector stems where known
 * (decodeXxxWithCursor:) so cross-referencing the dylib symbol table
 * is straightforward. Opcode 0x1a is dispatched from the sub-decoder
 * decodeRenderPassDescriptor:, not from the main jump table, but is
 * included here for completeness. */
typedef enum {
    /* --- Draw family (0x00-0x1d) ---------------------------------- */
    LAGFX_RENDER_OP_DRAW_PRIMITIVES_64                          = 0x00,
    LAGFX_RENDER_OP_DRAW_PRIMITIVES_16                          = 0x01,
    LAGFX_RENDER_OP_DRAW_INSTANCED_PRIMITIVES_64                = 0x02,
    LAGFX_RENDER_OP_DRAW_INSTANCED_PRIMITIVES_16                = 0x03,
    LAGFX_RENDER_OP_DRAW_INSTANCED_BASE_PRIMITIVES_64           = 0x04,
    LAGFX_RENDER_OP_DRAW_INSTANCED_BASE_PRIMITIVES_16           = 0x05,
    LAGFX_RENDER_OP_DRAW_INDEXED_PRIMITIVES_64                  = 0x06,
    LAGFX_RENDER_OP_DRAW_INDEXED_PRIMITIVES_16                  = 0x07,
    LAGFX_RENDER_OP_DRAW_INDEXED_INSTANCED_PRIMITIVES_64        = 0x08,
    LAGFX_RENDER_OP_DRAW_INDEXED_INSTANCED_PRIMITIVES_16        = 0x09,
    LAGFX_RENDER_OP_DRAW_INDEXED_INSTANCED_BASE_PRIMITIVES_64   = 0x0a,
    LAGFX_RENDER_OP_DRAW_INDEXED_INSTANCED_BASE_PRIMITIVES_16   = 0x0b,
    LAGFX_RENDER_OP_DRAW_PATCHES_64                             = 0x0c,
    LAGFX_RENDER_OP_DRAW_PATCHES_16                             = 0x0d,
    LAGFX_RENDER_OP_DRAW_INDEXED_PATCHES_64                     = 0x0e,
    LAGFX_RENDER_OP_DRAW_INDEXED_PATCHES_16                     = 0x0f,
    LAGFX_RENDER_OP_DRAW_PRIMITIVES_INDIRECT                    = 0x10,
    LAGFX_RENDER_OP_DRAW_INDEXED_PRIMITIVES_INDIRECT            = 0x11,
    LAGFX_RENDER_OP_DRAW_PATCHES_INDIRECT                       = 0x12,
    LAGFX_RENDER_OP_DRAW_INDEXED_PATCHES_INDIRECT               = 0x13,
    LAGFX_RENDER_OP_EXECUTE_COMMANDS_IN_BUFFER                  = 0x14,
    LAGFX_RENDER_OP_EXECUTE_COMMANDS_IN_BUFFER_RANGED           = 0x15,
    LAGFX_RENDER_OP_RENDER_BARRIER_RESOURCES                    = 0x16,
    LAGFX_RENDER_OP_RENDER_BARRIER_SCOPE                        = 0x17,
    LAGFX_RENDER_OP_RENDER_UPDATE_FENCE                         = 0x18,
    LAGFX_RENDER_OP_RENDER_WAIT_FOR_FENCE                       = 0x19,
    LAGFX_RENDER_OP_RENDER_DESCRIBE_RENDER_PASS                 = 0x1a,
    LAGFX_RENDER_OP_USE_HEAPS_WITH_STAGES                       = 0x1b,
    LAGFX_RENDER_OP_DRAW_INDEXED_INSTANCED_BASE_PRIMITIVES_64_2 = 0x1c,
    LAGFX_RENDER_OP_DRAW_INDEXED_INSTANCED_BASE_PRIMITIVES_16_2 = 0x1d,

    /* --- State-set family (0x65-0xa6) ----------------------------- */
    LAGFX_RENDER_OP_SET_BLEND_COLOR                             = 0x65,
    LAGFX_RENDER_OP_SET_COLOR_STORE_ACTION                      = 0x66,
    LAGFX_RENDER_OP_SET_COLOR_STORE_ACTION_OPTIONS              = 0x67,
    LAGFX_RENDER_OP_SET_DEPTH_STENCIL_STATE                     = 0x68,
    LAGFX_RENDER_OP_SET_DEPTH_STORE_ACTION                      = 0x69,
    LAGFX_RENDER_OP_SET_DEPTH_STORE_ACTION_OPTIONS              = 0x6a,
    LAGFX_RENDER_OP_SET_CULL_MODE                               = 0x6b,
    LAGFX_RENDER_OP_SET_DEPTH_BIAS                              = 0x6c,
    LAGFX_RENDER_OP_SET_DEPTH_CLIP_MODE                         = 0x6d,
    LAGFX_RENDER_OP_SET_FRAGMENT_BUFFERS                        = 0x6e,
    LAGFX_RENDER_OP_SET_FRAGMENT_BUFFER_OFFSET                  = 0x6f,
    LAGFX_RENDER_OP_SET_FRAGMENT_SAMPLER_STATES                 = 0x70,
    LAGFX_RENDER_OP_SET_FRAGMENT_SAMPLER_STATES_LOD_CLAMP       = 0x71,
    LAGFX_RENDER_OP_SET_FRAGMENT_TEXTURES                       = 0x72,
    LAGFX_RENDER_OP_SET_FRONT_FACING_WINDING                    = 0x73,
    LAGFX_RENDER_OP_SET_RENDER_PIPELINE_STATE                   = 0x74,
    LAGFX_RENDER_OP_SET_SCISSOR_RECT                            = 0x75,
    LAGFX_RENDER_OP_SET_SCISSOR_RECTS                           = 0x76,
    LAGFX_RENDER_OP_SET_STENCIL_REF                             = 0x77,
    LAGFX_RENDER_OP_SET_STENCIL_STORE_ACTION                    = 0x78,
    LAGFX_RENDER_OP_SET_STENCIL_STORE_ACTION_OPTIONS            = 0x79,
    LAGFX_RENDER_OP_SET_TESSELATION_FACTOR_BUFFER               = 0x7a,
    LAGFX_RENDER_OP_SET_TESSELATION_FACTOR_SCALE                = 0x7b,
    LAGFX_RENDER_OP_SET_TRIANGLE_FILL_MODE                      = 0x7c,
    LAGFX_RENDER_OP_SET_VERTEX_BUFFERS                          = 0x7d,
    LAGFX_RENDER_OP_SET_VERTEX_BUFFER_OFFSET                    = 0x7e,
    LAGFX_RENDER_OP_SET_VERTEX_SAMPLER_STATES                   = 0x7f,
    LAGFX_RENDER_OP_SET_VERTEX_SAMPLER_STATES_LOD_CLAMP         = 0x80,
    LAGFX_RENDER_OP_SET_VERTEX_TEXTURES                         = 0x81,
    LAGFX_RENDER_OP_SET_VIEWPORT                                = 0x82,
    LAGFX_RENDER_OP_SET_VIEWPORTS                               = 0x83,
    LAGFX_RENDER_OP_SET_VISIBILITY_RESULT_MODE                  = 0x84,
    LAGFX_RENDER_OP_TEXTURE_BARRIER                             = 0x85,
    LAGFX_RENDER_OP_USE_HEAPS                                   = 0x86,
    LAGFX_RENDER_OP_USE_RESOURCES                               = 0x87,
    LAGFX_RENDER_OP_SET_LINE_WIDTH                              = 0x88,
    LAGFX_RENDER_OP_USE_RESOURCES_WITH_STAGES                   = 0x89,
    LAGFX_RENDER_OP_SET_ALPHA_TEST_REFERENCE_VALUE              = 0x8a,
    LAGFX_RENDER_OP_SET_POINT_SIZE                              = 0x8b,
    LAGFX_RENDER_OP_SET_CLIP_PLANE                              = 0x8c,
    LAGFX_RENDER_OP_SET_VERTEX_SAMPLER_STATE                    = 0x8d,
    LAGFX_RENDER_OP_SET_FRAGMENT_SAMPLER_STATE                  = 0x8e,
    LAGFX_RENDER_OP_SET_VIEWPORT_TRANSFORM_ENABLED              = 0x8f,
    LAGFX_RENDER_OP_SET_PROVOKING_VERTEX_MODE                   = 0x90,
    LAGFX_RENDER_OP_SET_PRIMITIVE_RESTART_INDEX_ENABLED         = 0x91,
    LAGFX_RENDER_OP_SET_TRIANGLE_FILL_MODE_FRONT_BACK           = 0x92,
    LAGFX_RENDER_OP_SET_TRANSFORM_FEEDBACK_STATE                = 0x93,
    LAGFX_RENDER_OP_SET_DEPTH_CLEARED                           = 0x94,
    LAGFX_RENDER_OP_SET_STENCIL_CLEARED                         = 0x95,
    LAGFX_RENDER_OP_SET_COLOR_RESOLVE_TEXTURE                   = 0x96,
    LAGFX_RENDER_OP_SET_DEPTH_RESOLVE_TEXTURE                   = 0x97,
    LAGFX_RENDER_OP_SET_STENCIL_RESOLVE_TEXTURE                 = 0x98,
    LAGFX_RENDER_OP_SET_VERTEX_AMPLIFICATION_MODE               = 0x99,
    LAGFX_RENDER_OP_SET_VERTEX_AMPLIFICATION_COUNT              = 0x9a,
    LAGFX_RENDER_OP_DISPATCH_THREADS_PER_TILE                   = 0x9b,
    LAGFX_RENDER_OP_SET_RENDER_THREADGROUP_MEMORY_LENGTH        = 0x9c,
    LAGFX_RENDER_OP_SET_TILE_BUFFERS                            = 0x9d,
    LAGFX_RENDER_OP_SET_TILE_BUFFER_OFFSET                      = 0x9e,
    LAGFX_RENDER_OP_SET_TILE_SAMPLER_STATES                     = 0x9f,
    LAGFX_RENDER_OP_SET_TILE_SAMPLER_STATES_LOD_CLAMP           = 0xa0,
    LAGFX_RENDER_OP_SET_TILE_TEXTURES                           = 0xa1,
    LAGFX_RENDER_OP_DISPATCH_THREADS_PER_TILE_IN_REGION         = 0xa2,
    LAGFX_RENDER_OP_DISPATCH_THREADS_PER_TILE_IN_REGION_W_INDEX = 0xa3,
    LAGFX_RENDER_OP_GET_TILE_DIMENSIONS                         = 0xa4,
    LAGFX_RENDER_OP_SET_VERTEX_BUFFERS_WITH_STRIDE              = 0xa5,
    LAGFX_RENDER_OP_SET_VERTEX_BUFFER_OFFSET_WITH_STRIDE        = 0xa6,
} lagfx_render_op_t;

/* The on-wire opcode is u32; the per-decoder validity check
 * (`<= 0xa6` for Render — see inner-opcode-format.md §6) bounds it.
 * We expose the table count and a sentinel here. */
#define LAGFX_RENDER_OPCODE_COUNT  96u
#define LAGFX_RENDER_OPCODE_MAX    0xa6u

/* === Handler signature ========================================
 *
 * Per inner-opcode-format.md, the host decoder calls each handler with
 * a sub-cursor of length `payloadLen = totalLengthBytes - 8`. The
 * scaffolding gives the handler the flat (payload, len) pair so we
 * don't yet need to depend on the cursor abstraction. When real
 * handlers land they can layer a cursor on top trivially. */
typedef int (*lagfx_render_op_handler_fn)(lagfx_protocol_t *p,
                                          const uint8_t   *payload,
                                          size_t           len);

/* Descriptor for a single Render inner opcode. The four data fields
 * mirror the four data columns of render-decoder-handlers.tsv. */
typedef struct {
    uint32_t                  opcode;          /* on-wire opcode value */
    const char               *name;            /* TSV "name" column    */
    uint32_t                  body_size;       /* TSV "payload_size";
                                                * 0 if variable        */
    uint32_t                  ref_count;       /* TSV "ref_count"      */
    lagfx_render_op_handler_fn default_handler; /* current handler;
                                                 * starts as ack-only
                                                 * stub, replaced as
                                                 * real handlers land  */
} lagfx_render_op_descriptor_t;

/* === Table accessors =========================================== */

/* Look up a descriptor by on-wire opcode. Returns NULL if `opcode`
 * is not in the Render decoder's table. */
const lagfx_render_op_descriptor_t *
lagfx_render_op_lookup(uint32_t opcode);

/* Iterate the descriptor table — for tests / stats. */
size_t lagfx_render_op_table_size(void);
const lagfx_render_op_descriptor_t *
lagfx_render_op_table_entry(size_t index);

/* Short human-readable name for tracing. Returns "Unknown(0xNN)"
 * (in a static buffer) for opcodes not in the table. The buffer is
 * shared, so callers must not retain the pointer across calls. */
const char *lagfx_render_op_name(uint32_t opcode);

bool lagfx_render_op_is_stub(uint32_t opcode);

/* Return a pointer to the most recently parsed RenderDescribeRenderPass
 * (opcode 0x1a) descriptor for the given protocol instance. Valid
 * until the next 0x1a is dispatched on this protocol. Returns NULL on
 * a NULL protocol pointer. Per-protocol storage so concurrent
 * multi-device renderers don't alias each other's pass state. */
struct lagfx_protocol;
const lagfx_render_pass_desc_t *lagfx_render_pass_desc_get(
    const struct lagfx_protocol *p);

#endif /* LIBAPPLEGFX_PROTOCOL_RENDER_OPCODES_H */

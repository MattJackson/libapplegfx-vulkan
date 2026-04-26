/*
 * libapplegfx-vulkan — Render-decoder dispatch table (M5 scaffold)
 * src/protocol/render_opcodes.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Populates the 95-entry descriptor table for the Render inner-opcode
 * decoder. Every entry currently points at `render_op_ack_stub`, which
 * just logs and returns OK — real handlers land one-by-one as M5
 * progresses. Entries are listed in numerical opcode order (matches the
 * row order of paravirt-re/library/state-machines/render-decoder-
 * handlers.tsv).
 *
 * Source-of-truth row count: 95 (TSV body lines, excluding the header).
 *
 * Body sizes ("payload_size" in the TSV) are stored verbatim. Variable-
 * length entries (TSV value of the form "8+N*4") store 0 in `body_size`
 * — handlers that consume them parse the count themselves; the real
 * length arrives via the wire-level totalLengthBytes field, not the
 * descriptor.
 */

#include "render_opcodes.h"

#include "../common/log.h"

#include <stddef.h>
#include <stdio.h>

/* The default ack-only stub. Per the M5 brief: "Each stub just logs
 * `[lagfx render] op=0x%02x (%s) — ack-only stub`." We reach for
 * fprintf + LAGFX_LOG_ENABLED to keep the unconditional ack visible
 * even when LAGFX_LOG=0; the expectation is that during M5 bring-up
 * the operator wants to see when the dispatcher fired. If that proves
 * too noisy when the real handlers are landing, it's a one-line flip
 * to LAGFX_LOG. */
static int render_op_ack_stub(lagfx_protocol_t *p,
                              const uint8_t    *payload,
                              size_t            len) {
    (void)p;
    (void)payload;
    (void)len;
    /* The lookup happens in the dispatcher; the descriptor itself
     * carries the canonical name + opcode. We don't have access to
     * those here without adding a parameter, so the dispatcher logs
     * the formatted "[lagfx render] op=0xNN (name) — ack-only stub"
     * line just before invoking us. */
    return 0; /* OK */
}

/* === Descriptor table — 95 entries ============================
 *
 * Layout: { opcode, name, body_size, ref_count, default_handler }.
 *
 * `name` strings match the `name` column of the TSV exactly. */
static const lagfx_render_op_descriptor_t g_render_op_table[] = {
    /* --- Draw family (0x00-0x1d) -------------------------------- */
    { 0x00, "DrawPrimitives64",                          20, 0, render_op_ack_stub },
    { 0x01, "DrawPrimitives16",                           8, 0, render_op_ack_stub },
    { 0x02, "DrawInstancedPrimitives64",                 28, 0, render_op_ack_stub },
    { 0x03, "DrawInstancedPrimitives16",                  8, 0, render_op_ack_stub },
    { 0x04, "DrawInstancedBasePrimitives64",             36, 0, render_op_ack_stub },
    { 0x05, "DrawInstancedBasePrimitives16",             12, 0, render_op_ack_stub },
    { 0x06, "DrawIndexedPrimitives64",                   24, 1, render_op_ack_stub },
    { 0x07, "DrawIndexedPrimitives16",                   12, 1, render_op_ack_stub },
    { 0x08, "DrawIndexedInstancedPrimitives64",          32, 1, render_op_ack_stub },
    { 0x09, "DrawIndexedInstancedPrimitives16",          16, 1, render_op_ack_stub },
    { 0x0a, "DrawIndexedInstancedBasePrimitives64",      48, 1, render_op_ack_stub },
    { 0x0b, "DrawIndexedInstancedBasePrimitives16",      20, 1, render_op_ack_stub },
    { 0x0c, "DrawPatches64",                             48, 1, render_op_ack_stub },
    { 0x0d, "DrawPatches16",                             16, 1, render_op_ack_stub },
    { 0x0e, "DrawIndexedPatches64",                      60, 2, render_op_ack_stub },
    { 0x0f, "DrawIndexedPatches16",                      24, 2, render_op_ack_stub },
    { 0x10, "DrawPrimitivesIndirect",                    16, 1, render_op_ack_stub },
    { 0x11, "DrawIndexedPrimitivesIndirect",             28, 2, render_op_ack_stub },
    { 0x12, "DrawPatchesIndirect",                       28, 2, render_op_ack_stub },
    { 0x13, "DrawIndexedPatchesIndirect",                40, 3, render_op_ack_stub },
    { 0x14, "ExecuteCommandsInBuffer",                   16, 2, render_op_ack_stub },
    { 0x15, "ExecuteCommandsInBufferRanged",             20, 1, render_op_ack_stub },
    { 0x16, "RenderBarrierResources",                     0, 0, render_op_ack_stub },
    { 0x17, "RenderBarrierScope",                         4, 0, render_op_ack_stub },
    { 0x18, "RenderUpdateFence",                          8, 1, render_op_ack_stub },
    { 0x19, "RenderWaitForFence",                         8, 1, render_op_ack_stub },
    /* TSV body_size for 0x1b is "8+N*4" (variable) — store 0. */
    { 0x1b, "UseHeapsWithStages",                         0, 0, render_op_ack_stub },
    { 0x1c, "DrawIndexedInstancedBasePrimitives64_2",    48, 1, render_op_ack_stub },
    { 0x1d, "DrawIndexedInstancedBasePrimitives16_2",    20, 1, render_op_ack_stub },

    /* --- State-set family (0x65-0xa6) --------------------------- */
    { 0x65, "SetBlendColor",                             16, 0, render_op_ack_stub },
    { 0x66, "SetColorStoreAction",                        8, 0, render_op_ack_stub },
    { 0x67, "SetColorStoreActionOptions",                12, 0, render_op_ack_stub },
    { 0x68, "SetDepthStencilState",                       4, 1, render_op_ack_stub },
    { 0x69, "SetDepthStoreAction",                        8, 0, render_op_ack_stub },
    { 0x6a, "SetDepthStoreActionOptions",                 8, 0, render_op_ack_stub },
    { 0x6b, "SetCullMode",                                8, 0, render_op_ack_stub },
    { 0x6c, "SetDepthBias",                              12, 0, render_op_ack_stub },
    { 0x6d, "SetDepthClipMode",                           8, 0, render_op_ack_stub },
    /* 0x6e SetFragmentBuffers — variable "8+N*12". */
    { 0x6e, "SetFragmentBuffers",                         0, 0, render_op_ack_stub },
    { 0x6f, "SetFragmentBufferOffset",                   12, 0, render_op_ack_stub },
    /* 0x70 SetFragmentSamplerStates — variable "8+N*4". */
    { 0x70, "SetFragmentSamplerStates",                   0, 0, render_op_ack_stub },
    /* 0x71 SetFragmentSamplerStatesLODClamp — variable "8+N*12". */
    { 0x71, "SetFragmentSamplerStatesLODClamp",           0, 0, render_op_ack_stub },
    /* 0x72 SetFragmentTextures — variable "8+N*4". */
    { 0x72, "SetFragmentTextures",                        0, 0, render_op_ack_stub },
    { 0x73, "SetFrontFacingWinding",                      8, 0, render_op_ack_stub },
    { 0x74, "SetRenderPipelineState",                     4, 1, render_op_ack_stub },
    { 0x75, "SetScissorRect",                            32, 0, render_op_ack_stub },
    /* 0x76 SetScissorRects — variable "8+N*32". */
    { 0x76, "SetScissorRects",                            0, 0, render_op_ack_stub },
    { 0x77, "SetStencilRef",                              8, 0, render_op_ack_stub },
    { 0x78, "SetStencilStoreAction",                      8, 0, render_op_ack_stub },
    { 0x79, "SetStencilStoreActionOptions",               8, 0, render_op_ack_stub },
    { 0x7a, "SetTesselationFactorBuffer",                20, 1, render_op_ack_stub },
    { 0x7b, "SetTesselationFactorScale",                  4, 0, render_op_ack_stub },
    { 0x7c, "SetTriangleFillMode",                        8, 0, render_op_ack_stub },
    /* 0x7d SetVertexBuffers — variable "8+N*12". */
    { 0x7d, "SetVertexBuffers",                           0, 0, render_op_ack_stub },
    { 0x7e, "SetVertexBufferOffset",                     12, 0, render_op_ack_stub },
    /* 0x7f SetVertexSamplerStates — variable "8+N*4". */
    { 0x7f, "SetVertexSamplerStates",                     0, 0, render_op_ack_stub },
    /* 0x80 SetVertexSamplerStatesLODClamp — variable "8+N*12". */
    { 0x80, "SetVertexSamplerStatesLODClamp",             0, 0, render_op_ack_stub },
    /* 0x81 SetVertexTextures — variable "8+N*4". */
    { 0x81, "SetVertexTextures",                          0, 0, render_op_ack_stub },
    { 0x82, "SetViewport",                               48, 0, render_op_ack_stub },
    /* 0x83 SetViewports — variable "4+N*48". */
    { 0x83, "SetViewports",                               0, 0, render_op_ack_stub },
    { 0x84, "SetVisibilityResultMode",                   16, 0, render_op_ack_stub },
    { 0x85, "TextureBarrier",                             0, 0, render_op_ack_stub },
    /* 0x86 UseHeaps — variable "4+N*4". */
    { 0x86, "UseHeaps",                                   0, 0, render_op_ack_stub },
    /* 0x87 UseResources — variable "8+N*4". */
    { 0x87, "UseResources",                               0, 0, render_op_ack_stub },
    { 0x88, "SetLineWidth",                               4, 0, render_op_ack_stub },
    /* 0x89 UseResourcesWithStages — variable "8+N*4". */
    { 0x89, "UseResourcesWithStages",                     0, 0, render_op_ack_stub },
    { 0x8a, "SetAlphaTestReferenceValue",                 4, 0, render_op_ack_stub },
    { 0x8b, "SetPointSize",                               4, 0, render_op_ack_stub },
    { 0x8c, "SetClipPlane",                              20, 0, render_op_ack_stub },
    { 0x8d, "SetVertexSamplerState",                     20, 1, render_op_ack_stub },
    { 0x8e, "SetFragmentSamplerState",                   20, 1, render_op_ack_stub },
    { 0x8f, "SetViewportTransformEnabled",                4, 0, render_op_ack_stub },
    { 0x90, "SetProvokingVertexMode",                     4, 0, render_op_ack_stub },
    { 0x91, "SetPrimitiveRestartIndexEnabled",            8, 0, render_op_ack_stub },
    { 0x92, "SetTriangleFillModeFrontBack",               4, 0, render_op_ack_stub },
    { 0x93, "SetTransformFeedbackState",                  4, 0, render_op_ack_stub },
    { 0x94, "SetDepthCleared",                            0, 0, render_op_ack_stub },
    { 0x95, "SetStencilCleared",                          0, 0, render_op_ack_stub },
    { 0x96, "SetColorResolveTexture",                    16, 1, render_op_ack_stub },
    { 0x97, "SetDepthResolveTexture",                    12, 1, render_op_ack_stub },
    { 0x98, "SetStencilResolveTexture",                  12, 1, render_op_ack_stub },
    { 0x99, "SetVertexAmplificationMode",                 8, 0, render_op_ack_stub },
    /* 0x9a SetVertexAmplificationCount — variable "4+N*8". */
    { 0x9a, "SetVertexAmplificationCount",                0, 0, render_op_ack_stub },
    { 0x9b, "DispatchThreadsPerTile",                    24, 0, render_op_ack_stub },
    { 0x9c, "SetRenderThreadgroupMemoryLength",          20, 0, render_op_ack_stub },
    /* 0x9d SetTileBuffers — variable "8+N*12". */
    { 0x9d, "SetTileBuffers",                             0, 0, render_op_ack_stub },
    { 0x9e, "SetTileBufferOffset",                       12, 0, render_op_ack_stub },
    /* 0x9f SetTileSamplerStates — variable "8+N*4". */
    { 0x9f, "SetTileSamplerStates",                       0, 0, render_op_ack_stub },
    /* 0xa0 SetTileSamplerStatesLODClamp — variable "8+N*12". */
    { 0xa0, "SetTileSamplerStatesLODClamp",               0, 0, render_op_ack_stub },
    /* 0xa1 SetTileTextures — variable "8+N*4". */
    { 0xa1, "SetTileTextures",                            0, 0, render_op_ack_stub },
    { 0xa2, "DispatchThreadsPerTileInRegion",            76, 0, render_op_ack_stub },
    { 0xa3, "DispatchThreadsPerTileInRegionWithIndex",   76, 0, render_op_ack_stub },
    { 0xa4, "GetTileDimensions",                         12, 1, render_op_ack_stub },
    /* 0xa5 SetVertexBuffersWithStride — variable "8+N*20". */
    { 0xa5, "SetVertexBuffersWithStride",                 0, 0, render_op_ack_stub },
    { 0xa6, "SetVertexBufferOffsetWithStride",           20, 0, render_op_ack_stub },
};

static const size_t g_render_op_table_count =
    sizeof(g_render_op_table) / sizeof(g_render_op_table[0]);

/* Compile-time guarantee that the table size matches the public count
 * declared in render_opcodes.h. If the TSV ever grows/shrinks, the
 * header constant must be bumped in lockstep. */
_Static_assert(sizeof(g_render_op_table) / sizeof(g_render_op_table[0]) ==
               LAGFX_RENDER_OPCODE_COUNT,
               "render_opcodes.c table size must match "
               "LAGFX_RENDER_OPCODE_COUNT (95)");

const lagfx_render_op_descriptor_t *
lagfx_render_op_lookup(uint32_t opcode) {
    if (opcode > LAGFX_RENDER_OPCODE_MAX) {
        return NULL;
    }
    for (size_t i = 0; i < g_render_op_table_count; ++i) {
        if (g_render_op_table[i].opcode == opcode) {
            return &g_render_op_table[i];
        }
    }
    return NULL;
}

size_t lagfx_render_op_table_size(void) {
    return g_render_op_table_count;
}

const lagfx_render_op_descriptor_t *
lagfx_render_op_table_entry(size_t index) {
    if (index >= g_render_op_table_count) {
        return NULL;
    }
    return &g_render_op_table[index];
}

const char *lagfx_render_op_name(uint32_t opcode) {
    const lagfx_render_op_descriptor_t *d = lagfx_render_op_lookup(opcode);
    if (d) {
        return d->name;
    }
    /* Per src/protocol/protocol.h header comment, the decoder runs
     * single-threaded, so a file-static buffer is safe. */
    static char unknown_buf[24];
    snprintf(unknown_buf, sizeof(unknown_buf), "Unknown(0x%02x)",
             (unsigned)(opcode & 0xffu));
    return unknown_buf;
}

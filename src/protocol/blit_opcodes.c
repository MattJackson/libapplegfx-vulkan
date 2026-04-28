/*
 * libapplegfx-vulkan — Blit-decoder dispatch table (M5 scaffold)
 * src/protocol/blit_opcodes.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Populates the 24-entry descriptor table for the Blit inner-opcode
 * decoder. All entries point at `blit_op_ack_stub`; real handlers
 * land one-by-one as M5 progresses. Entries are listed in numerical
 * opcode order (matches the row order of
 * paravirt-re/library/state-machines/blit-decoder-handlers.tsv).
 *
 * Unlike the Render decoder, the Blit decoder has no variable-length
 * opcodes — every handler reads a single fixed-size PGCmd<Op> struct.
 * All body sizes come directly from the TSV's struct_size_bytes column.
 */

#include "blit_opcodes.h"

#include "../common/log.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int blit_op_ack_stub(lagfx_protocol_t *p,
                            const uint8_t    *payload,
                            size_t            len) {
    (void)p;
    (void)payload;
    (void)len;
    return 0;
}

bool lagfx_blit_op_is_stub(uint32_t opcode) {
    const lagfx_blit_op_descriptor_t *d = lagfx_blit_op_lookup(opcode);
    return d != NULL && d->default_handler == blit_op_ack_stub;
}

static const lagfx_blit_op_descriptor_t g_blit_op_table[] = {
    { 0x12c, "CopyFromBufferToTexture",                  88, 2, blit_op_ack_stub },
    { 0x12d, "CopyFromBufferToBuffer",                   32, 2, blit_op_ack_stub },
    { 0x12e, "CopyFromTextureToBuffer",                  88, 2, blit_op_ack_stub },
    { 0x12f, "CopyFromTextureToTexture",                 88, 2, blit_op_ack_stub },
    { 0x130, "CopyFromTextureToTextureWithOptions",      92, 2, blit_op_ack_stub },
    { 0x131, "CopyIndirectCommandBuffer",                32, 2, blit_op_ack_stub },
    { 0x132, "FillBuffer",                               24, 1, blit_op_ack_stub },
    { 0x133, "GenerateMipmaps",                           4, 1, blit_op_ack_stub },
    { 0x134, "OptimizeForCPUAccess",                      4, 1, blit_op_ack_stub },
    { 0x135, "OptimizeForGPUAccess",                      4, 1, blit_op_ack_stub },
    { 0x136, "OptimizeImageForCPUAccess",                 8, 1, blit_op_ack_stub },
    { 0x137, "OptimizeImageForGPUAccess",                 8, 1, blit_op_ack_stub },
    { 0x138, "OptimizeIndirectCommandBuffer",            20, 1, blit_op_ack_stub },
    { 0x139, "ResetCommandsInCommandBuffer",             20, 1, blit_op_ack_stub },
    { 0x13a, "SynchronizeResource",                       4, 1, blit_op_ack_stub },
    { 0x13b, "SynchronizeTextureImage",                   8, 1, blit_op_ack_stub },
    { 0x13c, "BlitUpdateFence",                           4, 1, blit_op_ack_stub },
    { 0x13d, "BlitWaitForFence",                          4, 1, blit_op_ack_stub },
    { 0x13e, "CopyFromTextureToTextureWithNumSliceLevel", 20, 2, blit_op_ack_stub },
    { 0x13f, "FillBufferWithPattern",                    24, 1, blit_op_ack_stub },
    { 0x140, "FillTextureWithBytes",                     76, 2, blit_op_ack_stub },
    { 0x141, "FillTextureWithColor",                     92, 1, blit_op_ack_stub },
    { 0x142, "InvalidateCompressedTexture",               4, 1, blit_op_ack_stub },
    { 0x143, "InvalidateCompressedTextureImage",          8, 1, blit_op_ack_stub },
};

static const size_t g_blit_op_table_count =
    sizeof(g_blit_op_table) / sizeof(g_blit_op_table[0]);

_Static_assert(sizeof(g_blit_op_table) / sizeof(g_blit_op_table[0]) ==
               LAGFX_BLIT_OPCODE_COUNT,
               "blit_opcodes.c table size must match "
               "LAGFX_BLIT_OPCODE_COUNT (24)");

const lagfx_blit_op_descriptor_t *
lagfx_blit_op_lookup(uint32_t opcode) {
    if (opcode < LAGFX_BLIT_OPCODE_MIN || opcode > LAGFX_BLIT_OPCODE_MAX) {
        return NULL;
    }
    size_t idx = opcode - LAGFX_BLIT_OPCODE_MIN;
    if (idx >= g_blit_op_table_count) {
        return NULL;
    }
    return &g_blit_op_table[idx];
}

size_t lagfx_blit_op_table_size(void) {
    return g_blit_op_table_count;
}

const lagfx_blit_op_descriptor_t *
lagfx_blit_op_table_entry(size_t index) {
    if (index >= g_blit_op_table_count) {
        return NULL;
    }
    return &g_blit_op_table[index];
}

const char *lagfx_blit_op_name(uint32_t opcode) {
    const lagfx_blit_op_descriptor_t *d = lagfx_blit_op_lookup(opcode);
    if (d) {
        return d->name;
    }
    static char unknown_buf[28];
    snprintf(unknown_buf, sizeof(unknown_buf), "Unknown(0x%03x)",
             (unsigned)(opcode & 0xffffu));
    return unknown_buf;
}

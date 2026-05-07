/*
 * libapplegfx-vulkan — Blit-decoder dispatch table (M5 phase 1)
 * src/protocol/blit_opcodes.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * IMPLEMENTATION STATUS (2026-05-07):
 *   🟡 FillTextureWithColor (0x141) — REAL handler via blit_encoder.c
 *   🟡 CopyFromTextureToTexture (0x12f) — REAL handler via blit_encoder.c
 *   ⚪ All other opcodes — STUBS (ack-only, no Vulkan translation)
 *
 * Phase 1 target: Wire minimum viable blits to produce visible pixels.
 * Future phases: Complete remaining 22 opcodes incrementally.
 */

#include "blit_opcodes.h"
#include "../translate/blit_encoder.h"

#include "state.h"
#include "../common/log.h"
#include "../device.h"
#include "resource_registry.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Metal types - already defined in blit_encoder.h */

/* Forward declarations for opcode handlers */
static int blit_op_fill_texture_with_color(lagfx_protocol_t *p,
                                           const uint8_t    *payload,
                                           size_t            len);
static int blit_op_copy_texture_to_texture(lagfx_protocol_t *p,
                                           const uint8_t    *payload,
                                           size_t            len);

/* ================================================================
 * Little-endian helpers
 * ================================================================ */

static inline uint32_t blit_le32(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static inline uint64_t blit_le64(const uint8_t *b) {
    return (uint64_t)blit_le32(b) | ((uint64_t)blit_le32(b + 4) << 32);
}

/* ================================================================
 * Ack-only stub for unimplemented opcodes
 * ================================================================ */

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

/* ================================================================
 * Blit-opcode dispatch table (Phase 1: minimum viable blits)
 * Layout: { opcode, name, body_size, ref_count, default_handler }.
 *
 * Phase 1 target (2026-05-07): Wire FillTextureWithColor + CopyFromTextureToTexture
 * to produce first visible pixels. All other opcodes remain stubs.
 * ================================================================ */

static const lagfx_blit_op_descriptor_t g_blit_op_table[] = {
    /* Phase 1: Minimum viable blits for first visible pixels */
    { 0x12c, "CopyFromBufferToTexture",                  88, 2, blit_op_ack_stub },
    { 0x12d, "CopyFromBufferToBuffer",                   32, 2, blit_op_ack_stub },
    { 0x12e, "CopyFromTextureToBuffer",                  88, 2, blit_op_ack_stub },
    { 0x12f, "CopyFromTextureToTexture",                 88, 2, blit_op_copy_texture_to_texture },
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
    { 0x141, "FillTextureWithColor",                     92, 1, blit_op_fill_texture_with_color },
    { 0x142, "InvalidateCompressedTexture",               4, 1, blit_op_ack_stub },
    { 0x143, "InvalidateCompressedTextureImage",          8, 1, blit_op_ack_stub },
};

/* ================================================================
 * 0x141 FillTextureWithColor — REAL HANDLER (Phase 1)
 *
 *   +0x00  u32 texture_ref        (resource registry index)
 *   +0x04  u32 level              (mip level)
 *   +0x08  u32 slice              (array slice / depth plane)
 *   +0x0c  MTLOrigin origin       (12 bytes: x, y, z as u32)
 *   +0x18  MTLSize size           (12 bytes: w, h, d as u32)
 *   +0x24  double color[4]        (32 bytes: RGBA doubles)
 *   +0x44  u32 pixel_format       (4 bytes: MTLPixelFormat)
 *   = 92 B total
 * ================================================================ */

static int blit_op_fill_texture_with_color(lagfx_protocol_t *p,
                                           const uint8_t    *payload,
                                           size_t            len) {
    if (!p || !payload || len < 92) {
        LAGFX_WARN("FillTextureWithColor: payload too short (%zu < 92)", len);
        return 0;
    }

    uint32_t texture_ref = blit_le32(payload + 0);
    uint32_t level       = blit_le32(payload + 4);
    uint32_t slice       = blit_le32(payload + 8);
    MTLOrigin origin     = {blit_le32(payload + 12),
                             blit_le32(payload + 16),
                             blit_le32(payload + 20)};
    MTLSize size         = {blit_le32(payload + 24),
                             blit_le32(payload + 28),
                             blit_le32(payload + 32)};
    double color[4];
    for (int i = 0; i < 4; ++i) {
        uint64_t u = blit_le64(payload + 36 + (size_t)i * 8);
        memcpy(&color[i], &u, sizeof(color[i]));
    }
    uint32_t pixel_format = blit_le32(payload + 68);

    LAGFX_LOG("FillTextureWithColor: ref=0x%x level=%u slice=%u "
               "origin=(%u,%u,%u) size=(%ux%ux%u) color=(%.3f,%.3f,%.3f,%.3f) fmt=0x%x",
               texture_ref, level, slice, origin.x, origin.y, origin.z,
               size.width, size.height, size.depth,
               (double)color[0], (double)color[1],
               (double)color[2], (double)color[3], pixel_format);

    if (!p->dev || !p->dev->vk || !p->dev->vk->initialized) {
        LAGFX_LOG("FillTextureWithColor: Vulkan not initialized — skipping");
        return 0;
    }

    /* Look up texture in resource registry */
    lagfx_resource_entry_t *entry =
        lagfx_resource_lookup(&p->resources, texture_ref, 0u);
    if (!entry || !entry->host_handle) {
        LAGFX_WARN("FillTextureWithColor: texture ref 0x%x not found in resource registry",
                    texture_ref);
        return 0;  // Fail-open: ack the stamp anyway
    }

    /* Call Vulkan translation layer */
    lagfx_status_t st = lagfx_translate_blit_fill_texture_color(
        p->dev->vk, entry->host_handle, level, slice, &origin, &size, color, pixel_format);

    if (st != LAGFX_OK) {
        LAGFX_WARN("FillTextureWithColor: Vulkan translation failed (%d)", (int)st);
    } else {
        LAGFX_LOG("FillTextureWithColor: Vulkan fill OK");
    }

    return 0;
}

/* ================================================================
 * 0x12f CopyFromTextureToTexture — REAL HANDLER (Phase 1)
 *
 *   +0x00  u32 src_texture_ref      (resource registry index)
 *   +0x08  u32 dst_texture_ref      (resource registry index)
 *   +0x10  MTLOrigin src_origin     (12 bytes: x, y, z as u32)
 *   +0x1c  MTLSize src_size         (12 bytes: w, h, d as u32)
 *   +0x28  u32 dst_slice            (4 bytes)
 *   +0x2c  u32 dst_level            (4 bytes)
 *   = 88 B total
 * ================================================================ */

static int blit_op_copy_texture_to_texture(lagfx_protocol_t *p,
                                           const uint8_t    *payload,
                                           size_t            len) {
    if (!p || !payload || len < 88) {
        LAGFX_WARN("CopyFromTextureToTexture: payload too short (%zu < 88)", len);
        return 0;
    }

    uint32_t src_ref   = blit_le32(payload + 0);
    uint32_t dst_ref   = blit_le32(payload + 4);
    MTLOrigin src_orig = {blit_le32(payload + 8),
                           blit_le32(payload + 12),
                           blit_le32(payload + 16)};
    MTLSize  src_size  = {blit_le32(payload + 20),
                           blit_le32(payload + 24),
                           blit_le32(payload + 28)};
    uint32_t dst_slice = blit_le32(payload + 32);
    uint32_t dst_level = blit_le32(payload + 36);

    LAGFX_LOG("CopyFromTextureToTexture: src=0x%x dst=0x%x "
               "src_orig=(%u,%u,%u) src_size=(%ux%ux%u) dst_slice=%u dst_level=%u",
               src_ref, dst_ref, src_orig.x, src_orig.y, src_orig.z,
               src_size.width, src_size.height, src_size.depth,
               dst_slice, dst_level);

    if (!p->dev || !p->dev->vk || !p->dev->vk->initialized) {
        LAGFX_LOG("CopyFromTextureToTexture: Vulkan not initialized — skipping");
        return 0;
    }

    /* Look up source texture in resource registry */
    lagfx_resource_entry_t *src_entry =
        lagfx_resource_lookup(&p->resources, src_ref, 0u);
    if (!src_entry || !src_entry->host_handle) {
        LAGFX_WARN("CopyFromTextureToTexture: src ref 0x%x not found in resource registry",
                    src_ref);
        return 0;
    }

    /* Look up dest texture in resource registry */
    lagfx_resource_entry_t *dst_entry =
        lagfx_resource_lookup(&p->resources, dst_ref, 0u);
    if (!dst_entry || !dst_entry->host_handle) {
        LAGFX_WARN("CopyFromTextureToTexture: dst ref 0x%x not found in resource registry",
                    dst_ref);
        return 0;
    }

    /* Call Vulkan translation layer */
    lagfx_status_t st = lagfx_translate_blit_copy_texture_to_texture(
        p->dev->vk, src_entry->host_handle, dst_entry->host_handle,
        &src_orig, &src_size, dst_slice, dst_level);

    if (st != LAGFX_OK) {
        LAGFX_WARN("CopyFromTextureToTexture: Vulkan translation failed (%d)", (int)st);
    } else {
        LAGFX_LOG("CopyFromTextureToTexture: Vulkan copy OK");
    }

    return 0;
}

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

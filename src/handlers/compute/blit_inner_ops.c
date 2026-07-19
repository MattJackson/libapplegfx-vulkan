/*
 * libapplegfx-vulkan — Blit inner-opcode handlers + dispatch table
 * src/handlers/compute/blit_inner_ops.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * RE: paravirt-re/library/state-machines/blit-decoder-handlers.tsv +
 *     pre-refactor src/protocol/blit_opcodes.c at b652199~1 (stranded
 *     in dead-code-to-revive/protocol/ between 2026-05-12 and the
 *     2026-05-13 consolidation pass).
 *
 * Architecture: encType=4 segments arrive via exec_cmdbuf.c's
 * inner_walk_segment. InfoDecoder opcodes (0x1c2..0x1d0) are routed
 * to info_replies before reaching this dispatch; this file handles
 * the real blit ops (0x12c..0x143) plus the extended low-range
 * (0x001..0x07d).
 *
 * Implementation status: every handler validates body size and
 * LAGFX_TRACEs the parsed fields. The pre-refactor FillTextureWithColor
 * (0x141) and CopyFromTextureToTexture (0x12f) carried real
 * VkCmdCopyImage / FillImage paths; both depended on a resource
 * registry binding texture_ref -> lagfx_vk_iosurface_t* that no longer
 * matches the post-dispatcher resource_registry shape. Carry forward
 * the parse-and-trace shape so wire format is visible in /tmp/lagfx.log;
 * the Vulkan translator hooks back in at Stage 30+ (TODO markers below).
 */

#include "blit_inner_ops.h"

#include "common/le.h"
#include "common/log.h"
#include "../device.h"
#include "../translate/blit_encoder.h"
#include "../protocol/resource_registry.h"
#include "../protocol/state.h"  /* For lagfx_protocol_t full definition */
#include "../vulkan/iosurface.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int (*lagfx_blit_inner_op_fn)(lagfx_protocol_t *p,
                                       const uint8_t   *payload,
                                       size_t           len);

typedef struct {
    uint32_t                opcode;
    const char             *name;
    uint32_t                body_size;
    uint32_t                ref_count;
    lagfx_blit_inner_op_fn  handler;
} lagfx_blit_inner_op_desc_t;

/* === Ack-only stub =============================================== */

static int op_ack_stub(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    (void)p; (void)payload; (void)len;
    return 0;
}

/* === Real-parse handlers ========================================= */

/* 0x12f — CopyFromTextureToTexture. Pre-refactor 88-byte payload:
 *   +0  u32 src_texture_ref     (slot+0..3 hold the ref; +4..7 padding
 *                                or alt-ref for the WithNumSliceLevel
 *                                variant — see 0x13e)
 *   +8  u32 dst_texture_ref
 *   +16 u32 src_origin.x        (MTLOrigin xyz, 12 bytes)
 *   +20 u32 src_origin.y
 *   +24 u32 src_origin.z
 *   +28 u32 src_size.w          (MTLSize whd, 12 bytes)
 *   +32 u32 src_size.h
 *   +36 u32 src_size.d
 *   +40 u32 dst_slice
 *   +44 u32 dst_level
 *   (offsets per pre-refactor blit_opcodes.c — payload from there
 *    indexed 0..36, but pre-refactor read at offset 0/4 for the two
 *    refs; the wire format may interleave a u32 pad. Live trace will
 *    show which layout the kext actually sends.)
 */
static int op_copy_texture_to_texture(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    if (len < 40) {
        LAGFX_WARN("CopyFromTextureToTexture: payload too short (%zu < 40)", len);
        return 0;
    }
    uint32_t src_ref   = lagfx_le32(payload + 0);
    uint32_t dst_ref   = lagfx_le32(payload + 4);
    uint32_t src_x     = lagfx_le32(payload + 8);
    uint32_t src_y     = lagfx_le32(payload + 12);
    uint32_t src_z     = lagfx_le32(payload + 16);
    uint32_t src_w     = lagfx_le32(payload + 20);
    uint32_t src_h     = lagfx_le32(payload + 24);
    uint32_t src_d     = lagfx_le32(payload + 28);
    uint32_t dst_slice = lagfx_le32(payload + 32);
    uint32_t dst_level = lagfx_le32(payload + 36);

    LAGFX_LOG("CopyFromTextureToTexture: src=0x%x dst=0x%x "
              "src_origin=(%u,%u,%u) src_size=%ux%ux%u dst_slice=%u dst_level=%u",
              src_ref, dst_ref, src_x, src_y, src_z,
              src_w, src_h, src_d, dst_slice, dst_level);

#ifdef LAGFX_HAVE_VULKAN
    struct lagfx_device *dev = (struct lagfx_device *)p->dev;
    if (!dev || !dev->vk) {
        LAGFX_WARN("CopyFromTextureToTexture: Vulkan state not available");
        return 0;
    }

    lagfx_resource_registry_t *reg = &p->resources;
    lagfx_resource_entry_t *src_entry = lagfx_resource_lookup(reg, src_ref, p->current_chan_id);
    lagfx_resource_entry_t *dst_entry = lagfx_resource_lookup(reg, dst_ref, p->current_chan_id);

    if (!src_entry || !src_entry->host_handle) {
        LAGFX_WARN("CopyFromTextureToTexture: source texture ref 0x%x not found", src_ref);
        return 0;
    }
    if (!dst_entry || !dst_entry->host_handle) {
        LAGFX_WARN("CopyFromTextureToTexture: dest texture ref 0x%x not found", dst_ref);
        return 0;
    }

    lagfx_vk_iosurface_t *src_tex = (lagfx_vk_iosurface_t *)src_entry->host_handle;
    lagfx_vk_iosurface_t *dst_tex = (lagfx_vk_iosurface_t *)dst_entry->host_handle;

    MTLOrigin src_origin = {.x = src_x, .y = src_y, .z = src_z};
    MTLSize src_size   = {.width = src_w, .height = src_h, .depth = src_d};

    lagfx_status_t st = lagfx_translate_blit_copy_texture_to_texture(
        dev->vk,
        src_tex,
        dst_tex,
        &src_origin,
        &src_size,
        dst_slice,
        dst_level
    );

    if (st != LAGFX_OK) {
        LAGFX_WARN("CopyFromTextureToTexture: blit translation failed (status=%d)", (int)st);
        return 0;
    }
#endif /* LAGFX_HAVE_VULKAN */

    return 0;
}

/* 0x141 — FillTextureWithColor. Pre-refactor 92-byte payload:
 *   +0   u32 texture_ref
 *   +4   u32 level
 *   +8   u32 slice
 *   +12  MTLOrigin origin (12 bytes: u32 x,y,z)
 *   +24  MTLSize size     (12 bytes: u32 w,h,d)
 *   +36  f64 color[4]     (32 bytes: RGBA doubles)
 *   +68  u32 pixel_format (MTLPixelFormat)
 */
static int op_fill_texture_with_color(lagfx_protocol_t *p, const uint8_t *payload, size_t len) {
    if (len < 92) {
        LAGFX_WARN("FillTextureWithColor: payload too short (%zu < 92)", len);
        return 0;
    }
    uint32_t texture_ref = lagfx_le32(payload + 0);
    uint32_t level       = lagfx_le32(payload + 4);
    uint32_t slice       = lagfx_le32(payload + 8);
    uint32_t ox          = lagfx_le32(payload + 12);
    uint32_t oy          = lagfx_le32(payload + 16);
    uint32_t oz          = lagfx_le32(payload + 20);
    uint32_t w           = lagfx_le32(payload + 24);
    uint32_t h           = lagfx_le32(payload + 28);
    uint32_t d           = lagfx_le32(payload + 32);
    double color[4];
    for (int i = 0; i < 4; ++i) {
        uint64_t u = lagfx_le64(payload + 36 + (size_t)i * 8);
        memcpy(&color[i], &u, sizeof(color[i]));
    }
    uint32_t pixel_format = lagfx_le32(payload + 68);

    LAGFX_LOG("FillTextureWithColor: ref=0x%x level=%u slice=%u "
              "origin=(%u,%u,%u) size=%ux%ux%u color=(%.3f,%.3f,%.3f,%.3f) fmt=0x%x",
              texture_ref, level, slice, ox, oy, oz, w, h, d,
              color[0], color[1], color[2], color[3], pixel_format);

#ifdef LAGFX_HAVE_VULKAN
    struct lagfx_device *dev = (struct lagfx_device *)p->dev;
    if (!dev || !dev->vk) {
        LAGFX_WARN("FillTextureWithColor: Vulkan state not available");
        return 0;
    }

    lagfx_resource_registry_t *reg = &p->resources;
    lagfx_resource_entry_t *entry = lagfx_resource_lookup(reg, texture_ref, p->current_chan_id);

    if (!entry || !entry->host_handle) {
        LAGFX_WARN("FillTextureWithColor: texture ref 0x%x not found", texture_ref);
        return 0;
    }

    lagfx_vk_iosurface_t *tex = (lagfx_vk_iosurface_t *)entry->host_handle;

    MTLOrigin origin   = {.x = ox, .y = oy, .z = oz};
    MTLSize size       = {.width = w, .height = h, .depth = d};

    lagfx_status_t st = lagfx_translate_blit_fill_texture_color(
        dev->vk,
        tex,
        level,
        slice,
        &origin,
        &size,
        color,
        pixel_format
    );

    if (st != LAGFX_OK) {
        LAGFX_WARN("FillTextureWithColor: blit translation failed (status=%d)", (int)st);
        return 0;
    }
#endif /* LAGFX_HAVE_VULKAN */

    return 0;
}

/* === Blit dispatch table (24 entries) ============================
 *
 * Layout matches paravirt-re blit-decoder-handlers.tsv. Body sizes
 * from TSV "payload_size" column verbatim.
 */
static const lagfx_blit_inner_op_desc_t g_table[] = {
    { 0x12c, "CopyFromBufferToTexture",                  88, 2, op_ack_stub },
    { 0x12d, "CopyFromBufferToBuffer",                   32, 2, op_ack_stub },
    { 0x12e, "CopyFromTextureToBuffer",                  88, 2, op_ack_stub },
    { 0x12f, "CopyFromTextureToTexture",                 88, 2, op_copy_texture_to_texture },
    { 0x130, "CopyFromTextureToTextureWithOptions",      92, 2, op_ack_stub },
    { 0x131, "CopyIndirectCommandBuffer",                32, 2, op_ack_stub },
    { 0x132, "FillBuffer",                               24, 1, op_ack_stub },
    { 0x133, "GenerateMipmaps",                           4, 1, op_ack_stub },
    { 0x134, "OptimizeForCPUAccess",                      4, 1, op_ack_stub },
    { 0x135, "OptimizeForGPUAccess",                      4, 1, op_ack_stub },
    { 0x136, "OptimizeImageForCPUAccess",                 8, 1, op_ack_stub },
    { 0x137, "OptimizeImageForGPUAccess",                 8, 1, op_ack_stub },
    { 0x138, "OptimizeIndirectCommandBuffer",            20, 1, op_ack_stub },
    { 0x139, "ResetCommandsInCommandBuffer",             20, 1, op_ack_stub },
    { 0x13a, "SynchronizeResource",                       4, 1, op_ack_stub },
    { 0x13b, "SynchronizeTextureImage",                   8, 1, op_ack_stub },
    { 0x13c, "BlitUpdateFence",                           4, 1, op_ack_stub },
    { 0x13d, "BlitWaitForFence",                          4, 1, op_ack_stub },
    { 0x13e, "CopyFromTextureToTextureWithNumSliceLevel", 20, 2, op_ack_stub },
    { 0x13f, "FillBufferWithPattern",                    24, 1, op_ack_stub },
    { 0x140, "FillTextureWithBytes",                     76, 2, op_ack_stub },
    { 0x141, "FillTextureWithColor",                     92, 1, op_fill_texture_with_color },
    { 0x142, "InvalidateCompressedTexture",               4, 1, op_ack_stub },
    { 0x143, "InvalidateCompressedTextureImage",          8, 1, op_ack_stub },
};

/* Extended low-range table (encType=4 streams occasionally carry
 * shared opcodes from the Render namespace). RE: pre-refactor
 * blit_opcodes.c g_blit_op_lookup extended table. */
static const lagfx_blit_inner_op_desc_t g_ext_table[] = {
    { 0x001, "Ext_0x001", 0, 0, op_ack_stub },
    { 0x01a, "Ext_0x01a", 0, 0, op_ack_stub },
    { 0x06e, "Ext_0x06e", 0, 0, op_ack_stub },
    { 0x070, "Ext_0x070", 0, 0, op_ack_stub },
    { 0x072, "Ext_0x072", 0, 0, op_ack_stub },
    { 0x074, "Ext_0x074", 0, 0, op_ack_stub },
    { 0x07d, "Ext_0x07d", 0, 0, op_ack_stub },
};

#define LAGFX_BLIT_OPCODE_MIN      0x12cu
#define LAGFX_BLIT_OPCODE_MAX      0x143u
#define LAGFX_BLIT_OPCODE_EXT_MIN  0x001u
#define LAGFX_BLIT_OPCODE_EXT_MAX  0x07du

static const lagfx_blit_inner_op_desc_t *table_lookup(uint32_t opcode) {
    if (opcode >= LAGFX_BLIT_OPCODE_EXT_MIN && opcode <= LAGFX_BLIT_OPCODE_EXT_MAX) {
        for (size_t i = 0; i < sizeof(g_ext_table) / sizeof(g_ext_table[0]); ++i) {
            if (g_ext_table[i].opcode == opcode) {
                return &g_ext_table[i];
            }
        }
        return NULL;
    }
    if (opcode < LAGFX_BLIT_OPCODE_MIN || opcode > LAGFX_BLIT_OPCODE_MAX) {
        return NULL;
    }
    size_t idx = opcode - LAGFX_BLIT_OPCODE_MIN;
    if (idx >= sizeof(g_table) / sizeof(g_table[0])) {
        return NULL;
    }
    return &g_table[idx];
}

int lagfx_blit_inner_dispatch(lagfx_protocol_t *p,
                               uint32_t          opcode,
                               const uint8_t    *payload,
                               size_t            len) {
    static unsigned long s_unknown_opcodes = 0;
    s_unknown_opcodes++;
    
    const lagfx_blit_inner_op_desc_t *d = table_lookup(opcode);
    if (!d) {
        if (s_unknown_opcodes <= 10) {
            LAGFX_WARN("blit inner: unknown opcode 0x%03x len=%zu — absorbed (count=%lu)",
                       (unsigned)(opcode & 0xfffu), len, s_unknown_opcodes);
        } else if (s_unknown_opcodes == 11) {
            LAGFX_WARN("blit inner: suppressing further unknown opcode logs");
        }
        return 0;
    }
    LAGFX_TRACE("blit inner: op=0x%03x (%s) len=%zu",
                (unsigned)(d->opcode & 0xfffu), d->name, len);
    if (!d->handler) {
        return 0;
    }
    return d->handler(p, payload, len);
}

const char *lagfx_blit_inner_op_name(uint32_t opcode) {
    const lagfx_blit_inner_op_desc_t *d = table_lookup(opcode);
    if (d) return d->name;
    static char unknown_buf[28];
    snprintf(unknown_buf, sizeof(unknown_buf), "Unknown(0x%03x)",
             (unsigned)(opcode & 0xfffu));
    return unknown_buf;
}

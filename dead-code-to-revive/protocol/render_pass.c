/*
 * libapplegfx-vulkan — RenderPassDescriptor parser (opcode 0x1a)
 * src/protocol/render_pass.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Parses the 584-byte PGCmdDescribeRenderPass wire struct into a
 * host-side lagfx_render_pass_desc_t. Called from the render-pass-
 * descriptor sub-decoder when opcode 0x1a is encountered.
 */

#include "render_pass.h"

#include "../common/log.h"

#include <string.h>

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p) {
    uint64_t lo = rd32(p);
    uint64_t hi = rd32(p + 4);
    return lo | (hi << 32);
}

static double rdf64(const uint8_t *p) {
    uint64_t v = rd64(p);
    double d;
    memcpy(&d, &v, sizeof(d));
    return d;
}

int lagfx_parse_render_pass_descriptor(const uint8_t *payload,
                                       size_t         len,
                                       lagfx_render_pass_desc_t *out) {
    if (!payload || !out) {
        LAGFX_ERR("parse_render_pass: NULL payload or out");
        return -1;
    }
    if (len < LAGFX_RENDER_PASS_WIRE_SIZE) {
        LAGFX_ERR("parse_render_pass: payload too short "
                  "(%zu < %u)", len, LAGFX_RENDER_PASS_WIRE_SIZE);
        return -1;
    }

    lagfx_render_pass_wire_t wire;
    memcpy(&wire, payload, sizeof(wire));

    memset(out, 0, sizeof(*out));

    out->has_depth = (rd32(wire.depth_texture_ref) != 0);
    if (out->has_depth) {
        out->depth_texture_ref         = rd32(wire.depth_texture_ref);
        out->depth_resolve_texture_ref = rd32(wire.depth_resolve_texture_ref);
        out->depth_level               = rd16(wire.depth_level);
        out->depth_slice               = rd16(wire.depth_slice);
        out->depth_load_action         = rd16(wire.depth_load_action);
        out->depth_store_action        = rd16(wire.depth_store_action);
        out->depth_clear_value         = rdf64(wire.depth_clear_depth);
    }

    out->has_stencil = (rd32(wire.stencil_texture_ref) != 0);
    if (out->has_stencil) {
        out->stencil_texture_ref         = rd32(wire.stencil_texture_ref);
        out->stencil_resolve_texture_ref = rd32(wire.stencil_resolve_texture_ref);
        out->stencil_load_action         = rd16(wire.stencil_load_action);
        out->stencil_store_action        = rd16(wire.stencil_store_action);
        out->stencil_clear_value         = rd32(wire.stencil_clear_stencil);
    }

    out->color_attachment_count = 0;
    for (unsigned i = 0; i < LAGFX_RENDER_PASS_MAX_COLOR_ATTACHMENTS; ++i) {
        if (rd32(wire.colors[i].texture_ref) == 0) {
            continue;
        }
        unsigned idx = out->color_attachment_count;
        out->colors[idx].texture_ref         = rd32(wire.colors[i].texture_ref);
        out->colors[idx].resolve_texture_ref = rd32(wire.colors[i].resolve_texture_ref);
        out->colors[idx].load_action         = rd16(wire.colors[i].load_action);
        out->colors[idx].store_action        = rd16(wire.colors[i].store_action);
        for (unsigned c = 0; c < 4; ++c) {
            out->colors[idx].clear_color[c] = rdf64(wire.colors[i].clear_color + c * 8);
        }
        out->color_attachment_count = idx + 1;
    }

    out->render_target_width         = rd64(wire.render_target_width);
    out->render_target_height        = rd64(wire.render_target_height);
    out->render_target_array_length  = rd64(wire.render_target_array_length);
    out->visibility_result_buffer_ref = rd32(wire.visibility_result_buffer_ref);

    return 0;
}

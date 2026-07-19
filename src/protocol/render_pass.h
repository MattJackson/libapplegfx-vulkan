/*
 * libapplegfx-vulkan — Parsed RenderPassDescriptor (opcode 0x1a)
 * src/protocol/render_pass.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Wire-format struct and parsed representation for the
 * PGCmdDescribeRenderPass inner opcode (0x1a), the 584-byte POD
 * aggregate that appears as the terminator of the render-pass-
 * descriptor sub-stream (opcodes 0x1a..0x24).
 *
 * Field layout reverse-engineered from
 * -[PGDeserializerRenderDecoder
 *    decodeRenderDescribeRenderPassWithCursor:descriptor:]
 * (arm64e disassembly at 0x22daec93c).
 *
 * Private to src/protocol/. Not installed.
 */

#ifndef LIBAPPLEGFX_PROTOCOL_RENDER_PASS_H
#define LIBAPPLEGFX_PROTOCOL_RENDER_PASS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LAGFX_RENDER_PASS_MAX_COLOR_ATTACHMENTS 8u
#define LAGFX_RENDER_PASS_WIRE_SIZE             0x248u /* 584 bytes */

/*
 * Single flat packed struct representing the entire 584-byte wire
 * payload. Field offsets verified against arm64e disassembly:
 *
 * Depth attachment: +0x00..+0x27
 *   +0x00  u32  texture_ref
 *   +0x04  u32  resolve_texture_ref
 *   +0x08  u16  level
 *   +0x0a  u16  slice
 *   +0x0c  u16  depth_plane
 *   +0x0e  u16  resolve_level
 *   +0x10  u16  resolve_slice
 *   +0x12  u16  resolve_depth_plane
 *   +0x14  u16  load_action
 *   +0x16  u16  store_action
 *   +0x18  u16  store_action_options
 *   +0x1a  u16  (reserved)
 *   +0x1c  f64  clear_depth
 *   +0x24  u16  depth_resolve_filter
 *   +0x26  u16  (reserved)
 *
 * Stencil attachment: +0x28..+0x49
 *   +0x28  u32  texture_ref
 *   +0x2c  u32  resolve_texture_ref
 *   +0x30  u16  level
 *   +0x32  u16  slice
 *   +0x34  u16  depth_plane
 *   +0x36  u16  resolve_level
 *   +0x38  u16  resolve_slice
 *   +0x3a  u16  resolve_depth_plane
 *   +0x3c  u16  load_action
 *   +0x3e  u16  store_action
 *   +0x40  u16  store_action_options
 *   +0x42  u16  (reserved)
 *   +0x44  u32  clear_stencil
 *   +0x48  u16  stencil_resolve_filter
 *   +0x4a  u16  (reserved)
 *
 * Color attachment array (8 entries, each 0x3c = 60 bytes): +0x4c..+0x22b
 *   Each entry:
 *     +0x00  u32  texture_ref
 *     +0x04  u32  resolve_texture_ref
 *     +0x08  u16  level
 *     +0x0a  u16  slice
 *     +0x0c  u16  depth_plane
 *     +0x0e  u16  resolve_level
 *     +0x10  u16  resolve_slice
 *     +0x12  u16  resolve_depth_plane
 *     +0x14  u16  load_action
 *     +0x16  u16  store_action
 *     +0x18  u16  store_action_options
 *     +0x1a  u16  (reserved)
 *     +0x1c  f64  clear_color[0]
 *     +0x24  f64  clear_color[1]
 *     +0x2c  f64  clear_color[2]
 *     +0x34  f64  clear_color[3]
 *
 * Trailing: +0x22c..+0x247
 *   +0x22c  u32  visibility_result_buffer_ref
 *   +0x230  u64  render_target_array_length
 *   +0x238  u64  render_target_width
 *   +0x240  u64  render_target_height
 */

typedef struct __attribute__((packed)) {
    uint8_t depth_texture_ref[4];
    uint8_t depth_resolve_texture_ref[4];
    uint8_t depth_level[2];
    uint8_t depth_slice[2];
    uint8_t depth_plane[2];
    uint8_t depth_resolve_level[2];
    uint8_t depth_resolve_slice[2];
    uint8_t depth_resolve_depth_plane[2];
    uint8_t depth_load_action[2];
    uint8_t depth_store_action[2];
    uint8_t depth_store_action_options[2];
    uint8_t _depth_reserved0[2];
    uint8_t depth_clear_depth[8];
    uint8_t depth_resolve_filter[2];
    uint8_t _depth_reserved1[2];

    uint8_t stencil_texture_ref[4];
    uint8_t stencil_resolve_texture_ref[4];
    uint8_t stencil_level[2];
    uint8_t stencil_slice[2];
    uint8_t stencil_plane[2];
    uint8_t stencil_resolve_level[2];
    uint8_t stencil_resolve_slice[2];
    uint8_t stencil_resolve_depth_plane[2];
    uint8_t stencil_load_action[2];
    uint8_t stencil_store_action[2];
    uint8_t stencil_store_action_options[2];
    uint8_t _stencil_reserved0[2];
    uint8_t stencil_clear_stencil[4];
    uint8_t stencil_resolve_filter[2];
    uint8_t _stencil_reserved1[2];

    struct __attribute__((packed)) {
        uint8_t texture_ref[4];
        uint8_t resolve_texture_ref[4];
        uint8_t level[2];
        uint8_t slice[2];
        uint8_t depth_plane[2];
        uint8_t resolve_level[2];
        uint8_t resolve_slice[2];
        uint8_t resolve_depth_plane[2];
        uint8_t load_action[2];
        uint8_t store_action[2];
        uint8_t store_action_options[2];
        uint8_t _reserved[2];
        uint8_t clear_color[32];
    } colors[LAGFX_RENDER_PASS_MAX_COLOR_ATTACHMENTS];

    uint8_t visibility_result_buffer_ref[4];
    uint8_t render_target_array_length[8];
    uint8_t render_target_width[8];
    uint8_t render_target_height[8];
} lagfx_render_pass_wire_t;

_Static_assert(sizeof(lagfx_render_pass_wire_t) == LAGFX_RENDER_PASS_WIRE_SIZE,
               "render pass wire: total 0x248 bytes");

typedef struct {
    bool     has_depth;
    uint32_t depth_texture_ref;
    uint32_t depth_resolve_texture_ref;
    uint16_t depth_level;
    uint16_t depth_slice;
    uint16_t depth_load_action;
    uint16_t depth_store_action;
    double   depth_clear_value;

    bool     has_stencil;
    uint32_t stencil_texture_ref;
    uint32_t stencil_resolve_texture_ref;
    uint16_t stencil_load_action;
    uint16_t stencil_store_action;
    uint32_t stencil_clear_value;

    uint32_t color_attachment_count;
    struct {
        uint32_t texture_ref;
        uint32_t resolve_texture_ref;
        uint16_t load_action;
        uint16_t store_action;
        double   clear_color[4];
    } colors[LAGFX_RENDER_PASS_MAX_COLOR_ATTACHMENTS];

    uint64_t render_target_width;
    uint64_t render_target_height;
    uint64_t render_target_array_length;
    uint32_t visibility_result_buffer_ref;
} lagfx_render_pass_desc_t;

int lagfx_parse_render_pass_descriptor(const uint8_t *payload,
                                       size_t         len,
                                       lagfx_render_pass_desc_t *out);

#endif /* LIBAPPLEGFX_PROTOCOL_RENDER_PASS_H */

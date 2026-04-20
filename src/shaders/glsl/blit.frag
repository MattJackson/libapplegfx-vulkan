/*
 * libapplegfx-vulkan — stock shader: blit (GLSL fragment)
 * src/shaders/glsl/blit.frag
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Twin of src/shaders/msl/blit.metal (fragment stage).
 *
 * Role: sample u_src at v_uv and output the texel unmodified.
 * The VkSampler the caller binds at set=0,binding=0 determines
 * nearest vs linear filtering — one shader serves both cases.
 *
 * Binding layout:
 *   set 0, binding 0: sampler2D u_src
 *                     - Metal twin: texture(0) + sampler(0)
 *
 * Attachment layout:
 *   location 0: vec4 out_color
 *               - written unconditionally (blit has no masking).
 *               - Format is the destination VkImage format; for
 *                 BGRA8_UNORM attachments the GPU takes care of
 *                 the channel swizzle automatically.
 */

#version 450

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D u_src;

void main() {
    out_color = texture(u_src, v_uv);
}

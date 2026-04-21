/*
 * libapplegfx-vulkan — stock shader: clear (GLSL fragment)
 * src/shaders/glsl/clear.frag
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Twin of src/shaders/msl/clear.metal (fragment stage).
 *
 * Role: emit a uniform clear colour to location 0 for every
 * fragment covered by the fullscreen triangle.
 *
 * Binding layout:
 *   set 0, binding 0: uniform ClearColor { vec4 color; } u_color
 *                     - Pushed via vkCmdPushDescriptorSetKHR
 *                       (Phase 3.A encoder) from the render-pass
 *                       clear-value-equivalent carried on the
 *                       guest transaction.
 *                     - Metal twin: constant float4 &color [[buffer(0)]]
 *
 * Attachment layout:
 *   location 0: vec4 out_color
 *               - format is the destination VkImage format; no
 *                 blending (SHADER_CLEAR is an opaque replace).
 */

#version 450

layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform ClearColor {
    vec4 color;
} u_color;

void main() {
    out_color = u_color.color;
}

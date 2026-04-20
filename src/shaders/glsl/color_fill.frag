/*
 * libapplegfx-vulkan — stock shader: color_fill (GLSL fragment)
 * src/shaders/glsl/color_fill.frag
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Twin of src/shaders/msl/color_fill.metal (fragment stage).
 *
 * Role: output u_color at every covered fragment. Covered area is
 * defined by the rectangle the vertex stage rasterises, so
 * color_fill is usable for any CALayer backgroundColor region
 * (not just full-attachment — that's what SHADER_CLEAR is for).
 *
 * Binding layout:
 *   set 0, binding 0: uniform FillColor { vec4 color; } u_color
 *                     - Metal twin: constant float4 &color [[buffer(0)]]
 *
 * Attachment layout:
 *   location 0: vec4 out_color
 *               - premultiplied; pipeline blend state decides
 *                 whether it's replace or OVER atop existing content.
 */

#version 450

layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform FillColor {
    vec4 color;
} u_color;

void main() {
    out_color = u_color.color;
}

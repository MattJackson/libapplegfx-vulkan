/*
 * libapplegfx-vulkan — stock shader: color_fill (GLSL fragment twin)
 * src/shaders/glsl/color_fill.frag
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Twin of src/shaders/msl/color_fill.metal (fragment stage).
 */

#version 450

layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform FillColor {
    vec4 color;
} u_color;

void main() {
    out_color = u_color.color;
}

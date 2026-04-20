/*
 * libapplegfx-vulkan — stock shader: color_fill (GLSL vertex twin)
 * src/shaders/glsl/color_fill.vert
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Twin of src/shaders/msl/color_fill.metal (vertex stage).
 */

#version 450

layout(location = 0) in vec2 in_position;

void main() {
    gl_Position = vec4(in_position, 0.0, 1.0);
}

/*
 * libapplegfx-vulkan — stock shader: composite_over (GLSL vertex twin)
 * src/shaders/glsl/composite_over.vert
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Twin of src/shaders/msl/composite_over.metal (vertex stage).
 */

#version 450

layout(location = 0) in vec2 in_position;
layout(location = 1) in vec2 in_uv;

layout(location = 0) out vec2 v_uv;

void main() {
    gl_Position = vec4(in_position, 0.0, 1.0);
    v_uv        = in_uv;
}

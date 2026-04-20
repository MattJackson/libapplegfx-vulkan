/*
 * libapplegfx-vulkan — stock shader: composite_over (GLSL fragment twin)
 * src/shaders/glsl/composite_over.frag
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Twin of src/shaders/msl/composite_over.metal (fragment stage).
 */

#version 450

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D u_layer;

void main() {
    out_color = texture(u_layer, v_uv);
}

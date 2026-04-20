/*
 * libapplegfx-vulkan — stock shader: color_fill (GLSL vertex)
 * src/shaders/glsl/color_fill.vert
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Twin of src/shaders/msl/color_fill.metal (vertex stage).
 *
 * Role: consumes a per-vertex 2D position stream (VB0) in NDC and
 * emits gl_Position. Typical use is a 2-triangle rectangle for a
 * CALayer backgroundColor layer — but any triangle list works.
 *
 * Vertex input layout (matches VkPipelineVertexInputStateCreateInfo
 * in the Phase 3.A encoder):
 *   location 0: vec2 in_position  (NDC xy)
 *
 * Binding layout: none at vertex stage. Fragment uses
 *   set 0, binding 0: uniform FillColor { vec4 color; } u_color
 */

#version 450

layout(location = 0) in vec2 in_position;

void main() {
    gl_Position = vec4(in_position, 0.0, 1.0);
}

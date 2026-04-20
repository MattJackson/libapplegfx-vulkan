/*
 * libapplegfx-vulkan — stock shader: composite_over (GLSL vertex)
 * src/shaders/glsl/composite_over.vert
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Twin of src/shaders/msl/composite_over.metal (vertex stage).
 *
 * Role: consumes a vertex buffer of (position, uv) pairs for a
 * composited layer rectangle and passes uv to the fragment stage.
 * Called once per 2-triangle layer quad.
 *
 * Vertex input layout (matches VkPipelineVertexInputStateCreateInfo
 * in the Phase 3.A encoder):
 *   location 0: vec2 in_position  (NDC xy; z=0, w=1 filled here)
 *   location 1: vec2 in_uv        (source-texture uv 0..1)
 *
 * Binding layout: none at vertex stage. Fragment uses
 *   set 0, binding 0: sampler2D u_src  (layer being composited)
 *   set 0, binding 1: sampler2D u_dst  (current framebuffer read-back)
 */

#version 450

layout(location = 0) in vec2 in_position;
layout(location = 1) in vec2 in_uv;

layout(location = 0) out vec2 v_uv;

void main() {
    gl_Position = vec4(in_position, 0.0, 1.0);
    v_uv        = in_uv;
}

/*
 * libapplegfx-vulkan — stock shader: blit (GLSL vertex twin)
 * src/shaders/glsl/blit.vert
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Twin of src/shaders/msl/blit.metal (vertex stage).
 * Fullscreen triangle from gl_VertexIndex.
 */

#version 450

layout(location = 0) out vec2 v_uv;

void main() {
    vec2 pos = vec2((gl_VertexIndex == 1) ?  3.0 : -1.0,
                    (gl_VertexIndex == 2) ?  3.0 : -1.0);
    gl_Position = vec4(pos, 0.0, 1.0);
    v_uv        = pos * 0.5 + 0.5;
}

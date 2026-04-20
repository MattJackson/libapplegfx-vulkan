/*
 * libapplegfx-vulkan — stock shader: clear (GLSL vertex twin)
 * src/shaders/glsl/clear.vert
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Twin of src/shaders/msl/clear.metal (vertex stage).
 */

#version 450

void main() {
    vec2 pos = vec2((gl_VertexIndex == 1) ?  3.0 : -1.0,
                    (gl_VertexIndex == 2) ?  3.0 : -1.0);
    gl_Position = vec4(pos, 0.0, 1.0);
}

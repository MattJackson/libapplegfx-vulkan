/*
 * libapplegfx-vulkan — stock shader: cursor (GLSL vertex twin)
 * src/shaders/glsl/cursor.vert
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Twin of src/shaders/msl/cursor.metal (vertex stage).
 * Triangle-strip quad for a cursor sprite.
 */

#version 450

layout(set = 0, binding = 0) uniform CursorParams {
    vec2 ndc_pos;
    vec2 ndc_size;
} u_params;

layout(location = 0) out vec2 v_uv;

void main() {
    /* vid=0 → (0,1)  vid=1 → (1,1)
     * vid=2 → (0,0)  vid=3 → (1,0) */
    vec2 corner = vec2(float((gl_VertexIndex & 1) != 0),
                       float((gl_VertexIndex & 2) == 0));
    gl_Position = vec4(u_params.ndc_pos + corner * u_params.ndc_size,
                       0.0, 1.0);
    v_uv        = vec2(corner.x, 1.0 - corner.y);
}

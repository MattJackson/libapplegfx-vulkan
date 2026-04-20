/*
 * libapplegfx-vulkan — stock shader: cursor (GLSL vertex)
 * src/shaders/glsl/cursor.vert
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Twin of src/shaders/msl/cursor.metal (vertex stage).
 *
 * Role: construct a triangle-strip quad in NDC from a uniform
 * (position, size) pair. No vertex buffer — gl_VertexIndex picks
 * the corner. Draw with vkCmdDraw(cmd, 4, 1, 0, 0) + primitive
 * topology TRIANGLE_STRIP.
 *
 * Binding layout:
 *   set 0, binding 0: uniform CursorParams {
 *       vec2 ndc_pos;   // top-left corner in NDC (-1..1)
 *       vec2 ndc_size;  // width + height in NDC
 *   } u_params
 *                     - Metal twin: constant CursorParams &params [[buffer(0)]]
 *   set 0, binding 1: sampler2D u_glyph   (used by cursor.frag)
 *                     - Metal twin: texture(0) + sampler(0)
 *
 * v_uv maps to the glyph texture with origin top-left; the flip
 * on corner.y matches BGRA-top-left glyph uploads (the cursor
 * bitmap arrives that way from the guest per
 * lagfx_display_callbacks_t::cursor_glyph).
 */

#version 450

layout(set = 0, binding = 0) uniform CursorParams {
    vec2 ndc_pos;
    vec2 ndc_size;
} u_params;

layout(location = 0) out vec2 v_uv;

void main() {
    /* Triangle-strip corner table driven by gl_VertexIndex:
     *   vid=0 → (0,1)  top-left
     *   vid=1 → (1,1)  top-right
     *   vid=2 → (0,0)  bottom-left
     *   vid=3 → (1,0)  bottom-right */
    vec2 corner = vec2(float((gl_VertexIndex & 1) != 0),
                       float((gl_VertexIndex & 2) == 0));
    gl_Position = vec4(u_params.ndc_pos + corner * u_params.ndc_size,
                       0.0, 1.0);
    /* Glyph uv: flip y so the uploaded BGRA top-left origin maps
     * onto our Vulkan y-down NDC output (the encoder applies a
     * negative-height viewport via VK_KHR_maintenance1). */
    v_uv        = vec2(corner.x, 1.0 - corner.y);
}

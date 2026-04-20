/*
 * libapplegfx-vulkan — stock shader: cursor (GLSL fragment)
 * src/shaders/glsl/cursor.frag
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Twin of src/shaders/msl/cursor.metal (fragment stage).
 *
 * Role: sample the cursor glyph texture and discard fully-
 * transparent fragments so the mouse pointer silhouette is sharp
 * rather than a translucent quad rectangle.
 *
 * The glyph texture is premultiplied BGRA per the guest upload
 * contract (lagfx_display_callbacks_t::cursor_glyph in the public
 * header). A caller-provided blend state (src=1, dst=1-srcA) on
 * the pipeline applies the final composite atop the current
 * framebuffer.
 *
 * Binding layout:
 *   set 0, binding 1: sampler2D u_glyph
 *                     - Metal twin: texture(0) + sampler(0)
 *                     - Note: binding index 1 (not 0) is used to
 *                       leave binding 0 for the CursorParams UBO
 *                       shared by the vertex stage.
 */

#version 450

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 1) uniform sampler2D u_glyph;

void main() {
    vec4 texel = texture(u_glyph, v_uv);
    if (texel.a <= 0.0) {
        /* Fully-transparent region of the cursor sprite — discard
         * so the framebuffer keeps its prior content. This matters
         * for non-rectangular cursors (hand, i-beam, resize arrows). */
        discard;
    }
    out_color = texel;
}

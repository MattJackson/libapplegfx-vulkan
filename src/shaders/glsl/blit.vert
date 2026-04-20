/*
 * libapplegfx-vulkan — stock shader: blit (GLSL vertex)
 * src/shaders/glsl/blit.vert
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Twin of src/shaders/msl/blit.metal (vertex stage).
 *
 * Role: emit a fullscreen triangle from gl_VertexIndex so the
 * fragment stage can sample `u_src` once per output pixel. No
 * vertex buffer required — draw via vkCmdDraw(cmd, 3, 1, 0, 0).
 *
 * Binding layout (matches blit.frag + MSL twin):
 *   set 0, binding 0: sampler2D u_src
 *                     - Metal: texture(0) + sampler(0)
 *                     - Nearest or linear filter chosen by the
 *                       caller's VkSampler. The shader is filter-
 *                       agnostic.
 *
 * Render-state expectations (set by the Phase 3.A encoder):
 *   - front face CCW, cull mode NONE (fullscreen triangle covers
 *     the viewport regardless of winding).
 *   - No depth test, no stencil.
 *   - Color attachment 0 = destination image, LOAD_OP_DONT_CARE
 *     (every pixel is overwritten).
 */

#version 450

layout(location = 0) out vec2 v_uv;

void main() {
    /* Fullscreen triangle trick:
     *   vid=0 → (-1, -1)  → uv (0, 0)
     *   vid=1 → ( 3, -1)  → uv (2, 0)
     *   vid=2 → (-1,  3)  → uv (0, 2)
     * The extra-large triangle is clipped at x=1 and y=1 giving a
     * perfect [-1,1]² fullscreen fill with no overdraw vs a 2-tri
     * quad. UVs are derived from NDC so (0,0) is top-left with
     * VK_VERTEX_INPUT_RATE_VERTEX gl_VertexIndex + Vulkan's default
     * clip-space y direction (handled by VK_KHR_maintenance1 /
     * negative-height viewport at the Phase 3.A encoder). */
    vec2 pos = vec2((gl_VertexIndex == 1) ?  3.0 : -1.0,
                    (gl_VertexIndex == 2) ?  3.0 : -1.0);
    gl_Position = vec4(pos, 0.0, 1.0);
    v_uv        = pos * 0.5 + 0.5;
}

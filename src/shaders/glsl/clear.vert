/*
 * libapplegfx-vulkan — stock shader: clear (GLSL vertex)
 * src/shaders/glsl/clear.vert
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Twin of src/shaders/msl/clear.metal (vertex stage).
 *
 * Role: fullscreen triangle from gl_VertexIndex. Used as the
 * shader-fallback clear path when a guest transaction doesn't map
 * cleanly onto VkAttachmentLoadOp.CLEAR (e.g., partial clears,
 * LOAD_OP_LOAD + frag fill). For the common full-attachment case
 * the Phase 2.B render-target path prefers the load-op clear; this
 * shader covers the fragment-fill fallback.
 *
 * No vertex inputs. Draw with vkCmdDraw(cmd, 3, 1, 0, 0).
 *
 * Binding layout: none in the vertex stage. Fragment uses
 *   set 0, binding 0: uniform ClearColor { vec4 color; }
 * via the paired clear.frag.
 */

#version 450

void main() {
    /* Same fullscreen-triangle pattern as blit.vert. */
    vec2 pos = vec2((gl_VertexIndex == 1) ?  3.0 : -1.0,
                    (gl_VertexIndex == 2) ?  3.0 : -1.0);
    gl_Position = vec4(pos, 0.0, 1.0);
}

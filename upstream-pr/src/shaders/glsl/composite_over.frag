/*
 * libapplegfx-vulkan — stock shader: composite_over (GLSL fragment)
 * src/shaders/glsl/composite_over.frag
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Twin of src/shaders/msl/composite_over.metal (fragment stage).
 *
 * Role: Porter-Duff OVER. Samples the source layer (u_src) and
 * the current framebuffer contents (u_dst) at v_uv and computes:
 *
 *     out.rgb = src.rgb + dst.rgb * (1 - src.a)
 *     out.a   = src.a   + dst.a   * (1 - src.a)
 *
 * This expects PREMULTIPLIED-ALPHA src — the standard CALayer
 * convention per CoreAnimation docs. The fragment returns the
 * composited colour already in premultiplied form so downstream
 * OVER compositing (layer atop layer atop layer) stays correct.
 *
 * NOTE on hardware-vs-shader blend: for the simple two-layer case
 * the GPU fixed-function blend stage (src=1, dst=1-srcA with
 * premultiplied) gives identical output with one fewer sample,
 * and is preferred when a caller can plumb it. This shader is the
 * fallback for the general n-layer CALayer tree where we cannot
 * use the single framebuffer as both attachment and sampled
 * texture (Vulkan rules forbid that) — in which case the caller
 * ping-pongs two scratch targets and we sample the previous
 * "destination" explicitly. See Phase 3.A encoder plan.
 *
 * Binding layout:
 *   set 0, binding 0: sampler2D u_src  (layer being composited in)
 *                     Metal twin: texture(0) + sampler(0)
 *   set 0, binding 1: sampler2D u_dst  (current-framebuffer read-back)
 *                     Metal twin: texture(1) + sampler(1)
 *
 * Attachment layout:
 *   location 0: vec4 out_color (premultiplied RGBA)
 */

#version 450

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D u_src;
layout(set = 0, binding = 1) uniform sampler2D u_dst;

void main() {
    vec4 src = texture(u_src, v_uv);
    vec4 dst = texture(u_dst, v_uv);
    float inv_src_a = 1.0 - src.a;
    out_color = vec4(src.rgb + dst.rgb * inv_src_a,
                     src.a   + dst.a   * inv_src_a);
}

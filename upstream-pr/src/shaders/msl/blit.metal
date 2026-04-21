/*
 * libapplegfx-vulkan — stock shader: blit (MSL)
 * src/shaders/msl/blit.metal
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Fullscreen-quad blit: samples a source texture and writes the
 * texel to the destination attachment. Our interpretation of
 * Apple's displayPresentVertex + displayPresentFragment pair
 * (the internal spec #3 + #4).
 *
 * Vertex stage emits a fullscreen triangle from vertex_id (no
 * vertex buffer needed). Fragment stage samples the bound texture
 * and returns the texel — the sampler is caller-provided so the
 * same shader handles both nearest and linear filtering.
 *
 * Binding layout (matches the GLSL twin bindings):
 *   texture(0) ←→ GLSL set=0, binding=0 (combined-sampler half)
 *   sampler(0) ←→ GLSL set=0, binding=0 (sampler half)
 *
 * Target: air64-apple-macos26.3 (catalog plan §3.1). Authored as
 * fresh MSL from the function name + paravirt-role — NOT
 * disassembled from Apple's AIR (catalog plan §6.2 licensing).
 */

#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

vertex VertexOut blit_vertex(uint vid [[vertex_id]]) {
    /* Emit a fullscreen triangle:
     *   vid=0 → (-1, -1)
     *   vid=1 → ( 3, -1)
     *   vid=2 → (-1,  3)
     * which rasterises to the full [-1, 1] NDC square and clips
     * outside. UVs are derived from the NDC coords. */
    VertexOut out;
    float2 pos = float2((vid == 1) ? 3.0 : -1.0,
                        (vid == 2) ? 3.0 : -1.0);
    out.position = float4(pos, 0.0, 1.0);
    out.uv       = pos * 0.5 + 0.5;
    return out;
}

fragment float4 blit_fragment(VertexOut in [[stage_in]],
                              texture2d<float> src [[texture(0)]],
                              sampler samp [[sampler(0)]]) {
    return src.sample(samp, in.uv);
}

/*
 * libapplegfx-vulkan — stock shader: blit (MSL)
 * src/shaders/msl/blit.metal
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Fullscreen-quad blit: samples a source texture and writes the
 * texel to the destination attachment. This is our interpretation
 * of Apple's displayPresentVertex + displayPresentFragment pair
 * (paravirt-re/metallib-analysis.md #3 + #4).
 *
 * Vertex stage emits a fullscreen triangle from gl_VertexID (no
 * vertex buffer needed). Fragment stage samples the bound texture
 * with a linear sampler and writes BGRA out.
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
     * outside. UVs are derived from the NDC coords so (0,0) is
     * the top-left of the screen. */
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

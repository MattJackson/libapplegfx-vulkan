/*
 * libapplegfx-vulkan — stock shader: composite_over (MSL)
 * src/shaders/msl/composite_over.metal
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Porter-Duff OVER compositing: samples a source layer's texture
 * with premultiplied alpha and composites atop the destination
 * layer read back from the previous framebuffer. Core CALayer
 * compositor primitive — every translucent layer above the root
 * (menu bar, dock, overlays) hits this shader.
 *
 * Formula (premultiplied alpha):
 *     out.rgb = src.rgb + dst.rgb * (1 - src.a)
 *     out.a   = src.a   + dst.a   * (1 - src.a)
 *
 * Vertex stage consumes a (position, uv) vertex stream — typically
 * a 2-triangle layer quad per draw. The encoder's Phase 3.A
 * bind_pipeline path binds VB0 to the layer-geometry buffer before
 * vkCmdDraw.
 *
 * Binding layout (matches the GLSL twin):
 *   texture(0) + sampler(0)  ←→  GLSL set=0, binding=0 (u_src)
 *   texture(1) + sampler(1)  ←→  GLSL set=0, binding=1 (u_dst)
 */

#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

struct VertexIn {
    float2 position [[attribute(0)]];
    float2 uv       [[attribute(1)]];
};

vertex VertexOut composite_over_vertex(VertexIn in [[stage_in]]) {
    VertexOut out;
    out.position = float4(in.position, 0.0, 1.0);
    out.uv       = in.uv;
    return out;
}

fragment float4 composite_over_fragment(VertexOut in [[stage_in]],
                                        texture2d<float> src [[texture(0)]],
                                        sampler src_samp     [[sampler(0)]],
                                        texture2d<float> dst [[texture(1)]],
                                        sampler dst_samp     [[sampler(1)]]) {
    float4 s = src.sample(src_samp, in.uv);
    float4 d = dst.sample(dst_samp, in.uv);
    float inv_src_a = 1.0 - s.a;
    return float4(s.rgb + d.rgb * inv_src_a,
                  s.a   + d.a   * inv_src_a);
}

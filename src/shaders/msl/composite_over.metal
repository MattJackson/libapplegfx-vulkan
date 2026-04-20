/*
 * libapplegfx-vulkan — stock shader: composite_over (MSL)
 * src/shaders/msl/composite_over.metal
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Porter-Duff OVER compositing: samples a source layer's texture
 * with premultiplied alpha and composites atop the destination.
 * This is the core CALayer compositor primitive — every
 * translucent layer above the root (menu bar, dock, overlays)
 * hits this shader.
 *
 * We rely on the GPU blend stage (premultiplied-alpha src + 1-srcA
 * dst) to do the mixing — the shader itself just returns the
 * sampled colour. This keeps the shader small and lets pipeline
 * state (Phase 3.E) carry the blend configuration per-draw.
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
                                        texture2d<float> layer [[texture(0)]],
                                        sampler samp [[sampler(0)]]) {
    /* Sampled texel is premultiplied-alpha BGRA — blend stage
     * takes it from here. */
    return layer.sample(samp, in.uv);
}

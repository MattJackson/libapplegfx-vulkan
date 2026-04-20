/*
 * libapplegfx-vulkan — stock shader: color_fill (MSL)
 * src/shaders/msl/color_fill.metal
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Solid-colour fill of a rectangular region. Distinct from
 * clear.metal in that it consumes a vertex buffer (the rectangle
 * geometry) rather than emitting a fullscreen triangle — this is
 * the path CALayer backgroundColor layers travel when the layer
 * is smaller than the full attachment.
 *
 * Inputs: VB0 is a stream of 2D positions (triangle list) in NDC.
 * Uniform buffer 0 carries the fill RGBA.
 */

#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float4 position [[position]];
};

struct VertexIn {
    float2 position [[attribute(0)]];
};

vertex VertexOut color_fill_vertex(VertexIn in [[stage_in]]) {
    VertexOut out;
    out.position = float4(in.position, 0.0, 1.0);
    return out;
}

fragment float4 color_fill_fragment(VertexOut in [[stage_in]],
                                    constant float4 &color [[buffer(0)]]) {
    (void)in;
    return color;
}

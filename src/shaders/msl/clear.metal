/*
 * libapplegfx-vulkan — stock shader: clear (MSL)
 * src/shaders/msl/clear.metal
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Draws a constant colour over the full attachment. Used when a
 * CALayer compositor emits a clear-like fill that the paravirt
 * plumbing does not translate to VkAttachmentLoadOp.CLEAR
 * (fixed-function clear is Phase 2's path — this is the fallback
 * for paths that route the clear through the draw stream).
 *
 * Fragment push-constants / buffer(0) carry the RGBA to emit.
 * Vertex is the same fullscreen-triangle trick as blit.metal.
 */

#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float4 position [[position]];
};

vertex VertexOut clear_vertex(uint vid [[vertex_id]]) {
    VertexOut out;
    float2 pos = float2((vid == 1) ? 3.0 : -1.0,
                        (vid == 2) ? 3.0 : -1.0);
    out.position = float4(pos, 0.0, 1.0);
    return out;
}

fragment float4 clear_fragment(VertexOut in [[stage_in]],
                               constant float4 &color [[buffer(0)]]) {
    (void)in;
    return color;
}

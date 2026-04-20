/*
 * libapplegfx-vulkan — stock shader: clear (MSL)
 * src/shaders/msl/clear.metal
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Fragment-shader clear path. Distinct from VK_ATTACHMENT_LOAD_OP_CLEAR
 * (which Phase 2.B prefers whenever the guest transaction maps 1:1
 * to a full-attachment clear) — this is the fallback for partial
 * clears that the paravirt plumbing cannot rewrite as a load-op.
 *
 * Vertex stage emits the same fullscreen triangle as blit.metal.
 * Fragment returns the uniform colour from buffer(0).
 *
 * Binding layout (matches the GLSL twin):
 *   buffer(0) (fragment) ←→ GLSL set=0,binding=0 UBO { vec4 color; }
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

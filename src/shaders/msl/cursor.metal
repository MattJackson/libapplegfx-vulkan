/*
 * libapplegfx-vulkan — stock shader: cursor (MSL)
 * src/shaders/msl/cursor.metal
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Cursor glyph rasteriser: draws the cursor sprite at the current
 * position, sampling the glyph texture (32-bit BGRA with
 * premultiplied alpha per the lagfx_display_callbacks_t
 * cursor_glyph shape in libapplegfx-vulkan.h).
 *
 * Vertex stage consumes a 2D quad (4 verts, triangle strip).
 * Uniform buffer(0) carries (pos.xy, size.xy) in NDC.
 * Fragment samples + discards transparent fragments so the mouse
 * silhouette is precise.
 *
 * Binding layout (matches the GLSL twin):
 *   buffer(0) (vertex)             ←→ GLSL set=0,binding=0 CursorParams UBO
 *   texture(0) + sampler(0) (frag) ←→ GLSL set=0,binding=1 sampler2D
 */

#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

struct CursorParams {
    float2 ndc_pos;   /* top-left corner in NDC (-1..1) */
    float2 ndc_size;  /* width + height in NDC */
};

vertex VertexOut cursor_vertex(uint vid [[vertex_id]],
                               constant CursorParams &params [[buffer(0)]]) {
    /* Triangle-strip quad: 4 vertices.
     *   vid=0 → (0,1)  top-left
     *   vid=1 → (1,1)  top-right
     *   vid=2 → (0,0)  bottom-left
     *   vid=3 → (1,0)  bottom-right */
    float2 corner = float2(float((vid & 1u) != 0u),
                           float((vid & 2u) == 0u));
    VertexOut out;
    out.position = float4(params.ndc_pos + corner * params.ndc_size,
                          0.0, 1.0);
    out.uv       = float2(corner.x, 1.0 - corner.y);
    return out;
}

fragment float4 cursor_fragment(VertexOut in [[stage_in]],
                                texture2d<float> glyph [[texture(0)]],
                                sampler samp [[sampler(0)]]) {
    float4 t = glyph.sample(samp, in.uv);
    if (t.a <= 0.0) {
        /* MSL discards via discard_fragment(); keeps the
         * behaviour identical to the GLSL twin. */
        discard_fragment();
    }
    return t;
}

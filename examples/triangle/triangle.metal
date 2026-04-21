// examples/triangle/triangle.metal
// libapplegfx-vulkan — minimal single-triangle MSL shader (Phase 3.C.2 E2E)
//
// Pair of vertex + fragment functions that together draw a single
// colored triangle. Designed to be small enough that the AIR bitcode
// emitted by `xcrun metal -c` is easy to retarget + lower to SPIR-V
// via LLVM's SPIR-V backend (`llc -mtriple=spirv64-unknown-vulkan1.3`).
//
// The vertex stage uses a hard-coded 3-vertex clip-space triangle
// indexed by vertex_id so there are no vertex buffer bindings to
// plumb through air2spirv. The fragment stage writes a fixed red
// color so the test can assert on the centre pixel.

#include <metal_stdlib>
using namespace metal;

struct VOut {
    float4 position [[position]];
};

vertex VOut triangle_vertex(uint vid [[vertex_id]]) {
    // Big centred triangle. Clip space covers [-1, 1] in XY.
    const float2 verts[3] = {
        float2( 0.0f,  0.75f),
        float2(-0.75f, -0.75f),
        float2( 0.75f, -0.75f),
    };
    VOut out;
    out.position = float4(verts[vid], 0.0f, 1.0f);
    return out;
}

fragment float4 triangle_fragment() {
    // Opaque red. Readback code asserts the centre pixel is red.
    return float4(1.0f, 0.0f, 0.0f, 1.0f);
}

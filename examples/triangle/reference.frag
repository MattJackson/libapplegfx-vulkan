// examples/triangle/reference.frag
// libapplegfx-vulkan — GLSL reference fragment shader (Phase 3.C.2 E2E).
//
// Companion fallback to triangle.metal::triangle_fragment. Opaque red,
// identical semantics so the lavapipe readback check asserts on the
// same centre-pixel colour regardless of which SPIR-V blob is bound.

#version 450

layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(1.0, 0.0, 0.0, 1.0);
}

// examples/triangle/reference.vert
// libapplegfx-vulkan — GLSL reference vertex shader (Phase 3.C.2 E2E).
//
// Companion fallback to triangle.metal. Compiles to Vulkan-flavour
// SPIR-V via glslang. Used by tests/triangle-lavapipe-e2e.c to prove
// that the test harness + lavapipe pipeline can bind + draw a triangle
// while the AIR->SPIR-V scaffold stabilises the entry-point metadata
// rewrite (see paravirt-re/shader-llvm-spirv-poc-runbook.md §12).
//
// Geometry matches triangle.metal so the readback assertions apply
// identically to both paths.

#version 450

void main() {
    vec2 verts[3] = vec2[](
        vec2( 0.0,  0.75),
        vec2(-0.75, -0.75),
        vec2( 0.75, -0.75)
    );
    gl_Position = vec4(verts[gl_VertexIndex], 0.0, 1.0);
}

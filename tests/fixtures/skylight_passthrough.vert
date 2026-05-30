#version 450
/*
 * Reference vertex for the SkyLight lavapipe render gate
 * (tests/skylight-lavapipe-render.c). Emits a fullscreen-ish triangle and
 * feeds the real SkyLight `SimpleColorFragment` its stage_in:
 *   Location 0 -> the fragment's unused [[position]] input (dummy)
 *   Location 1 -> the interpolated colour (constant green) the fragment
 *                 returns to the render target.
 */
layout(location = 0) out vec4 v_pos_unused;
layout(location = 1) out vec4 v_color;

void main() {
    vec2 verts[3] = vec2[](vec2(0.0, 0.75), vec2(-0.75, -0.75), vec2(0.75, -0.75));
    gl_Position  = vec4(verts[gl_VertexIndex], 0.0, 1.0);
    v_pos_unused = gl_Position;
    v_color      = vec4(0.0, 1.0, 0.0, 1.0);  /* green */
}

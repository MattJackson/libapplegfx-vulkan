#version 450
/*
 * Reference vertex for the SkyLight TEXTURE render gate
 * (tests/skylight-lavapipe-render.c --tex mode). Emits a fullscreen-ish
 * triangle and feeds the real SkyLight texture fragment its stage_in:
 *   Location 0 -> the fragment's unused [[position]] input (dummy vec4)
 *   Location 1 -> the interpolated tex coord (constant 0.5,0.5 = texture
 *                 centre, so a solid-colour texture samples to that colour).
 */
layout(location = 0) out vec4 v_pos_unused;
layout(location = 1) out vec2 v_uv;

void main() {
    vec2 verts[3] = vec2[](vec2(0.0, 0.75), vec2(-0.75, -0.75), vec2(0.75, -0.75));
    gl_Position  = vec4(verts[gl_VertexIndex], 0.0, 1.0);
    v_pos_unused = gl_Position;
    v_uv         = vec2(0.5, 0.5);
}

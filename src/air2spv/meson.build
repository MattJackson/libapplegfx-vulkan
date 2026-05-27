# libapplegfx-vulkan — clean-room AIR -> SPIR-V translator (Phase 4+)
# src/air2spv/meson.build
#
# Separate directory from src/air2spirv (the OLD triangle-only
# translator). Phase 4 emitter scaffolding lives here.

air2spv_sources = files(
  'spv_builder.c',
  'emit_position.c',
  'emit_vertex_id.c',
  'emit_render_target.c',
  'emit_fragment_input.c',
  'emit_buffer_load.c',
  'emit_constant_float.c',
  'emit_texture_sample.c',
  'emit_extinst_glsl.c',
  'emit_vector_shuffle.c',
  'emit_control_flow.c',
  'emit_loop.c',
  'translate.c',
  'translate_function.c',
)

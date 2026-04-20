# src/translate/meson.build — Metal-to-Vulkan translation layer (Phase 3.A)
#
# Called from src/meson.build via subdir('translate'). Appends the
# render-encoder TU to lagfx_sources. The TU builds unconditionally;
# LAGFX_HAVE_VULKAN gating lives inside the C source so the no-vulkan
# Darwin dev path still compiles.
#
# See src/translate/render_encoder.h for the API shape and
# paravirt-re/phase-3-metal-vulkan-plan.md §3.A for the broader
# translator scope.

lagfx_sources += files(
  'render_encoder.c',
)

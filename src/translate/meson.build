# src/translate/meson.build — Metal-to-Vulkan translation layer (M5 phase 1)
#
# Called from src/meson.build via subdir('translate'). Appends the
# render-encoder and blit-encoder TUs to lagfx_sources. The TUs build
# unconditionally; LAGFX_HAVE_VULKAN gating lives inside the C source
# so the no-vulkan Darwin dev path still compiles.
#
# M5 phase 1 (2026-05-07): Wire FillTextureWithColor + CopyFromTextureToTexture
# to produce first visible pixels through lavapipe.

lagfx_sources += files(
  'render_encoder.c',
  'blit_encoder.c',
)

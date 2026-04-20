# src/vulkan/meson.build — Vulkan init TU (Phase 1.B)
#
# Called from src/meson.build. Appends instance.c to lagfx_sources.
# The LAGFX_HAVE_VULKAN define is added in the top-level meson.build
# when vulkan_dep.found() is true; this file just contributes the
# source unconditionally (the file itself #ifdefs around the symbol).

lagfx_sources += files(
  'instance.c',
  'command.c',
)

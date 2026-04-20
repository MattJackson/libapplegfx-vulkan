# src/meson.build — core library sources (lifecycle stubs)
#
# Called from top-level meson.build. Appends to the parent's
# lagfx_sources list.

lagfx_sources += files(
  'device.c',
  'display.c',
  'mmio.c',
  'version.c',
)

# Private internal headers live alongside the .c files; they are NOT
# installed. subdir() calls for subsystem-local sources go here.

subdir('common')
subdir('protocol')
subdir('vulkan')
subdir('shaders')

# src/meson.build — core library sources (lifecycle stubs)
#
# Called from top-level meson.build. Appends to the parent's
# lagfx_sources list.

lagfx_sources += files(
  'device.c',
  'display.c',
  'doorbell.c',        # New: BAR0 write listener and dispatcher routing
  'version.c',
)

# Private internal headers live alongside the .c files; they are NOT
# installed. subdir() calls for subsystem-local sources go here.

subdir('common')
subdir('timer')     # Host timer callbacks (Phase 1.A online event)
subdir('protocol')
subdir('dispatchers')
subdir('handlers')
subdir('vulkan')
subdir('shaders')
subdir('translate')
subdir('air2spirv')
subdir('metallib')

# src/protocol/meson.build — protocol decoder sources (Phase 1.A.2)
#
# Appends to the parent lagfx_sources list. Paths must match §2 of
# mos/the internal spec.

lagfx_sources += files(
  'protocol.c',
  'fifo.c',
  'opcodes.c',
  'ops_misc.c',
  'ops_device.c',
  'ops_queue.c',
  'ops_cmdbuf.c',
  'ops_display.c',
  'ops_iosurface.c',
)

# state.h / protocol.h / opcodes.h / fifo.h are private to src/protocol/
# — not installed, reachable via the internal_include_dir declared in
# the top-level meson.build.

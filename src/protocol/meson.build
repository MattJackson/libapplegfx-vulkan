# src/protocol/meson.build — protocol decoder sources (Phase 1.A.2)
#
# Appends to the parent lagfx_sources list. Paths must match §2 of
# mos/paravirt-re/phase-1a2-decoder-plan.md.

lagfx_sources += files(
  'protocol.c',
  'translate.c',
  'fifo.c',
  'opcodes.c',
  'ops_misc.c',
  'ops_device.c',
  'ops_queue.c',
  'ops_cmdbuf.c',
  'ops_display.c',
  'ops_iosurface.c',
  'resource_registry.c',
  'blit_opcodes.c',
  'blit_decoder.c',
  'compute_opcodes.c',
  'compute_decoder.c',
  'render_opcodes.c',
  'render_decoder.c',
  'render_pass.c',
  'ops_display_vchan.c',
)

# Dispatcher implementations (OOP-style polymorphic dispatch)
lagfx_sources += files(
  '../dispatchers/base.h',
  '../dispatchers/registry.h',
  '../dispatchers/registry.c',
  '../dispatchers/compute_dispatcher.c',
  '../dispatchers/display_vchan_dispatcher.c',
  '../dispatchers/channel_0_dispatcher.h',
  '../dispatchers/channel_0_dispatcher.c',
  '../dispatchers/channel_1_dispatcher.h',
  '../dispatchers/channel_1_dispatcher.c',
  '../dispatchers/channel_2_dispatcher.h',
  '../dispatchers/channel_2_dispatcher.c',
  '../dispatchers/channel_3_dispatcher.h',
  '../dispatchers/channel_3_dispatcher.c',
  '../dispatchers/channel_4_dispatcher.h',
  '../dispatchers/channel_4_dispatcher.c',
  '../dispatchers/unknown_dispatcher.c',
)

# Handler implementations (logic layer, no routing code)
lagfx_sources += files(
  '../handlers/handlers.h',
  '../handlers/compute/exec_indirect2.c',
  '../handlers/task/task.c',
  '../handlers/memory/memory.c',
  '../handlers/display/display.c',
  '../handlers/sync/sync.c',
  '../handlers/util/util.c',
)

# state.h / protocol.h / opcodes.h / fifo.h / render_opcodes.h /
# render_decoder.h / render_pass.h are private to src/protocol/ — not
# installed, reachable via the internal_include_dir declared in the
# top-level meson.build.

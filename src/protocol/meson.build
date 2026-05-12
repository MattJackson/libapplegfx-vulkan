# src/protocol/meson.build — protocol decoder sources (Phase 1.A.2)
#
# Appends to the parent lagfx_sources list. Paths must match §2 of
# mos/paravirt-re/phase-1a2-decoder-plan.md.

lagfx_sources += files(
  'protocol.c',
  'translate.c',
  'fifo.c',
  'opcodes.c',
  'ops_device.c',      # Device operations (define_task, delete_task, etc.)
  'ops_queue.c',       # Queue operations  
  'ops_display.c',     # Display operations (cursor, transaction3, etc.)
  'ops_iosurface.c',   # iOSurface operations
  'ops_misc.c',        # Miscellaneous operations
  'ops_cmdbuf.c',      # Command buffer execution + synchronize_resources
  'blit_opcodes.c',    # Blit opcode table + lookup functions
  'compute_opcodes.c', # Compute opcode table + lookup functions
  'render_opcodes.c',  # Render opcode table + lookup functions
  'resource_registry.c',
  'blit_decoder.c',
  'compute_decoder.c',
  'render_decoder.c',
  'render_pass.c',
)

# Note: ops_*.c files still needed - opcodes.c references functions in these files

# Dispatcher implementations (OOP-style polymorphic dispatch)
lagfx_sources += files(
  '../dispatchers/base.h',
  '../dispatchers/registry.h',
  '../dispatchers/registry.c',
  '../dispatchers/channel_5_plus_dispatcher.c',
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

# Handler implementations (logic layer, no routing code) - single unified handler file
lagfx_sources += files(
  '../handlers/handlers.h',
  '../handlers/compute/exec_cmdbuf.c',
  '../handlers/task/task.c',
  '../handlers/memory/memory.c',
  '../handlers/display/display.c',  # All display handlers (vchan + regular)
  '../handlers/sync/sync.c',
  '../handlers/util/util.c',
)

# state.h / protocol.h / opcodes.h / fifo.h / render_opcodes.h /
# render_decoder.h / render_pass.h are private to src/protocol/ — not
# installed, reachable via the internal_include_dir declared in the
# top-level meson.build.

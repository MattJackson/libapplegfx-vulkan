# src/protocol/meson.build — Protocol state machine sources
#
# Copyright © 2026 Matthew Jackson
# SPDX-License-Identifier: AGPL-3.0-or-later

lagfx_sources += files(
  'state.h',
  'lifecycle.c',    # new/free/reset
  'stamp.c',        # advance_stamp_cell, complete_stamp_slot
  'resource_registry.c',  # resource mapping table (CmdMapMemory2, etc.)
  'ops_misc.c',     # NOP, Debug, GetDeviceInfo handlers
  # ops_display.c removed: uses old protocol state layout (lagfx_display_entry_t renamed to lagfx_display_mode_t)
  # Legacy handlers moved to src/handlers/display/display.c for vchan opcodes (0x01-0x07)
  'test_api.c',     # Test API exports: dispatch_one, mmio_read/write
   'object_resolver.c',  # Phase B object ID resolver for metallib lookup
)

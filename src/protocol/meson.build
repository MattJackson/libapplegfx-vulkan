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
  'protocol.c',     # dispatch_one, mmio_read/write functions
)

# ops_display.c uses old protocol state layout; removed during handler refactor
# TODO: update to use new protocol architecture before re-enabling

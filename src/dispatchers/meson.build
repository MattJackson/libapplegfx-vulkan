# src/dispatchers/meson.build — Dispatcher routing sources
#
# Copyright © 2026 Matthew Jackson
# SPDX-License-Identifier: MIT

lagfx_sources += files(
  'primary_ring_door_dispatcher.h',
  'primary_ring_door_dispatcher.c',   # BAR0+0x1008 → primary ring drain
  'channel_door_dispatcher.h',
  'channel_door_dispatcher.c',        # BAR0+0x1020 → routes by channel ID
  'channel_0_dispatcher.h',
  'channel_0_dispatcher.c',           # Root channel sub-dispatcher
  'channel_compute_dispatcher.h',
  'channel_compute_dispatcher.c',     # Compute channels (ch 1-4) sub-dispatcher
  'channel_display_dispatcher.h',
  'channel_display_dispatcher.c',     # Display channels (ch 5+) sub-dispatcher — routes vchan opcodes 0x01-0x07
  'ring_common.h',
  'ring_common.c',                     # Shared FIFORingDescriptor + PFN-array helpers (used by compute/display drains)
)

# channel_5_plus_dispatcher.c removed: references old protocol state fields not present in new architecture

# src/handlers/meson.build — Handler execution sources
#
# Copyright © 2026 Matthew Jackson
# SPDX-License-Identifier: AGPL-3.0-or-later

lagfx_sources += files(
  'handlers.h',
  
  # Task handlers (opcode 0x00, 0x01)
  'task/task.c',
  
  # Memory handlers (opcode 0x02, 0x03)
  'memory/memory.c',
  
  # Compute handler (opcode 0x20)
  'compute/exec_cmdbuf.c',
  'compute/info_replies.c',
  
  # Sync handler (opcode 0x42)
  'sync/sync.c',
  
  # Display handlers (opcode 0x10-0x1a)
  'display/display.c',
  
  # Utility handlers (NOP, Debug)
  'util/util.c',
)

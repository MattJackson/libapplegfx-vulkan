# src/handlers/meson.build — Handler execution sources
#
# Copyright © 2026 Matthew Jackson
# SPDX-License-Identifier: MIT

lagfx_sources += files(
  'handlers.h',
  
  # Task handlers (opcode 0x00, 0x01)
  'task/task.c',
  
  # Memory handlers (opcode 0x02, 0x03)
  'memory/memory.c',
  
  # Compute handler (opcode 0x20)
  'compute/exec_cmdbuf.c',
  'compute/info_replies.c',
  'compute/render_inner_ops.c',   # encType=2 inner-opcode table (96 entries)
  'compute/render_pass.c',        # 0x1a RenderPassDescriptor parser
  'compute/blit_inner_ops.c',     # encType=4 blit inner-opcode table (24+7)
  'compute/compute_inner_ops.c',  # encType=0/1 inner-opcode dispatch (Stage 30 Task 6 prep)

  # IOSurface outer-FIFO handlers (opcodes 0x26-0x2a)
  'iosurface/iosurface.c',
  
  # Sync handler (opcode 0x42)
  'sync/sync.c',
  
  # Display handlers (opcode 0x10-0x1a)
  'display/display.c',
  
  # Utility handlers (NOP, Debug)
  'util/util.c',
)

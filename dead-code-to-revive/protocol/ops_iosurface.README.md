# ops_iosurface.c

**LOC:** 519.

**What it is:** Real handlers for the IOSurface opcode family
(0x26/0x27/0x28/0x29 + a couple of immediate-vchan extras). Tracks
last-seen IOSurface state per opcode for tests, and registers
guest IOSurface backings with the protocol's resource registry.

**Symbols provided:**
- `lagfx_op_iosurface_create_backing2()` — 0x27, real-mode
- `lagfx_op_iosurface_delete_backing2()` — 0x26
- `lagfx_op_iosurface_lookup()` — 0x28
- `lagfx_op_iosurface_import_mach_port()` — kext-extra
- `lagfx_op_iosurface_unmap()` — kext-extra
- `lagfx_ops_iosurface_last_create / last_delete / last_lookup /
  last_update / last_import / last_unmap` — test accessors
- `lagfx_ops_iosurface_reset()`

**Revival trigger:** M5 stage 25 — when macOS opens its first real
IOSurface backing for SkyLight composition we need real registration
(not just ack-and-drop). The dispatcher today logs and acks; that's
fine for kext bring-up but won't survive WindowServer.

**Known struct-drift on revival:**
- Uses pre-refactor `lagfx_iosurface_capture_t` (defined in
  `src/protocol/ops_iosurface.h`). Header still exists; check
  whether the resource registry's API surface has changed
  (`src/protocol/resource_registry.c/h` was kept LIVE through the
  refactor).
- Several handlers were written against the old `state.h` task
  layout — cross-check task lookups.

**Forward-port preference:** Port into `src/handlers/iosurface/` (new
dir). Wire into `channel_0_dispatcher.c` since IOSurface ops are
issued on the root channel.

**Last-alive SHA:** `b652199`.

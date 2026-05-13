# dead-code-to-revive/protocol/

Forward-port material from `src/protocol/`. These `.c` files were
dropped from `meson.build` during the dispatcher refactor
(`b8d1166`, 2026-05-12) and the pre-refactor opcode-table dispatch
model was replaced by per-channel dispatchers in `src/dispatchers/`
plus per-domain handlers in `src/handlers/`.

The seven files preserved here carry **render-path, IOSurface,
blit-path, and render-pass material** that we'll need when Stage 25
(IOSurface registration end-to-end) and Stage 30 (first visible
pixel via render pipeline) come online. Their logic has **not**
been ported into the live tree yet — they're the canonical
implementations.

## Inventory

| File | LOC | Stage | Revival trigger |
|---|---|---|---|
| `render_opcodes.c` | 2948 | M5 stage 30 | First Metal draw command needs the 95-entry render-op descriptor table |
| `ops_iosurface.c` | 519 | M5 stage 25/30 | macOS sends `CmdIOSurfaceCreate` (0x27) / `CmdIOSurfaceLookup` (0x28) / `CmdIOSurfaceUpdate` (0x29) and we need real backing-store registration (not just ack) |
| `blit_opcodes.c` | 402 | M5 stage 30+ | First BlitCommandEncoder dispatch needs the blit-op descriptor table |
| `render_pass.c` | 101 | M5 stage 25/30 | Parse `MTLRenderPassDescriptor` from `CmdSetRenderPipelineState`'s sibling payload |
| `render_decoder.c` | 65 | M5 stage 25/30 | Inner-opcode dispatch for the render command segment |
| `compute_decoder.c` | 55 | M5 stage 25/30 | Inner-opcode dispatch for the compute command segment |
| `blit_decoder.c` | 55 | M5 stage 25/30 | Inner-opcode dispatch for the blit command segment |

## Revival procedure

1. Read the per-file `*.README.md` for the specific incompatibilities
   (struct-drift, signature changes) flagged at file-move time.
2. Cross-check against current `src/protocol/state.h` and
   `src/protocol/opcodes.h` — the wire-protocol header
   `lagfx_cmd_header_t` is stable but per-task / per-channel state
   shapes drift.
3. Decide whether to port into `src/handlers/<domain>/` (preferred —
   matches the post-refactor architecture) or restore under
   `src/protocol/`. New code should follow the
   handlers/dispatchers split.
4. Re-add to the relevant `meson.build` only after the build passes
   `meson compile -C build` cleanly.
5. Wire into the appropriate channel dispatcher in
   `src/dispatchers/`.

## Pre-refactor SHA

`b652199bc4e4f42cfc2801b15c7be84c183c0baa` — last commit where all
these files were in `meson.build` and compiling.
`b8d1166d92d312e17df7d07f7a912e7289b21b21` — the dispatcher refactor
that removed them from the build.

## What's NOT here

The other six pre-refactor `.c` files (`opcodes.c`, `ops_device.c`,
`ops_display.c`, `ops_queue.c`, `fifo.c`, `translate.c`,
`compute_opcodes.c`) were **deleted** outright — their logic is fully
covered by the live dispatcher / handler tree:

| Deleted file | Superseded by |
|---|---|
| `opcodes.c` | per-channel dispatch in `src/dispatchers/*_dispatcher.c` |
| `ops_device.c` | `src/handlers/{task,memory}/*.c` + `channel_0_dispatcher.c` |
| `ops_display.c` | `src/handlers/display/display.c` + `channel_display_dispatcher.c` + `src/display.c` cursor stubs |
| `ops_queue.c` | DefineChildFifo / DeleteChildFifo inlined in `channel_0_dispatcher.c` |
| `fifo.c` | Ring drain in `primary_ring_door_dispatcher.c` |
| `translate.c` | Radix VA→GPA walk in `src/handlers/compute/exec_cmdbuf.c::task_translate` |
| `compute_opcodes.c` | `channel_compute_dispatcher.c` |

If you need any of those, retrieve from git at SHA `b652199`.

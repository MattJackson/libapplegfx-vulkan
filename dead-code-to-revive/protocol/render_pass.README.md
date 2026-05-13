# render_pass.c

**LOC:** 101.

**What it is:** Parser for the on-wire `MTLRenderPassDescriptor`
payload — turns the variable-length wire format (color attachments
0..7, depth, stencil, viewport, scissor, sample-count, etc.) into
the host's `lagfx_render_pass_desc_t` struct.

**Symbols provided:**
- `lagfx_parse_render_pass_descriptor(payload, len, out)` — the
  one and only entry point.

**Revival trigger:** M5 stage 25/30 — render_opcodes.c (above) calls
this to materialise the descriptor cached on the protocol state for
each `RenderPassDescriptor` (inner opcode 0x1a). Both files travel
together; revive together.

**Known struct-drift on revival:**
- The `lagfx_render_pass_desc_t` shape is defined in
  `src/protocol/render_pass.h` (still in tree). Cross-check that
  the live `src/translate/render_encoder.c` is happy with the
  field set — those two consume the same struct.
- The live `src/handlers/compute/exec_cmdbuf.c` already has a
  reference *comment* mentioning `lagfx_parse_render_pass_descriptor`
  at the spot where the inner-opcode dispatch will need it.

**Forward-port preference:** Port into `src/handlers/render/`
alongside `render_opcodes.c` material. Keep as a single small TU.

**Last-alive SHA:** `b652199`.

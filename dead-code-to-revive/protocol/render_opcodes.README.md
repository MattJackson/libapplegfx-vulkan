# render_opcodes.c

**LOC:** 2948 — by far the largest stranded TU.

**What it is:** The Stage 30 render path. Carries the 95-entry
render-op descriptor table (`g_render_op_table[]`), per-opcode stub
predicates (`lagfx_render_op_is_stub`), the render-pass-descriptor
parse + cache (`lagfx_render_pass_desc_get`), and the inner-opcode
implementations for the Render encoder family in the
`CmdExecIndirect2` segment stream.

**Symbols provided:**
- `lagfx_render_op_table_size()`
- `lagfx_render_op_lookup(opcode)`
- `lagfx_render_op_table_entry(index)`
- `lagfx_render_op_is_stub(opcode)`
- `lagfx_render_pass_desc_get(protocol)`
- `lagfx_render_encoder_try_begin(protocol)` — forward declared,
  intended to call into `src/translate/render_encoder.c`.

**Revival trigger:** M5 stage 30 — first visible pixel. When macOS
sends real `MTLRenderPassDescriptor` payloads through inner opcode
0x1a (RenderPassDescriptor) and we need to dispatch the 95 render
inner ops (drawPrimitives, setVertexBuffer, etc.) into our Vulkan
render-encoder.

**Known struct-drift on revival:**
- Uses pre-refactor `lagfx_protocol_t` field set. Cross-check
  `state.h` — task storage moved to a per-protocol table; the inner
  parsers may need to take a `lagfx_task_entry_t *` directly.
- Calls `lagfx_render_encoder_try_begin()` which has never been
  defined; it's a placeholder for the call into the live
  `src/translate/render_encoder.c` (different family — the live
  Vulkan-side encoder).
- Includes `../translate/render_encoder.h` — that path is still
  valid post-refactor.

**Forward-port preference:** Port the descriptor table + parser
implementations into `src/handlers/render/render.c` (new); wire
into `channel_compute_dispatcher.c` (which currently handles render
opcodes too — see the dispatcher's TODO comments).

**Last-alive SHA:** `b652199` (parent of dispatcher refactor
`b8d1166`).

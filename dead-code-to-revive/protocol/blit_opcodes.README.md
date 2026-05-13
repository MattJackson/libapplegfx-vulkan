# blit_opcodes.c

**LOC:** 402.

**What it is:** Stage 30+ blit path. The descriptor table for the
24-entry blit inner-opcode family (`copyFromTexture`,
`copyFromBuffer`, `generateMipmaps`, `synchronizeResource`, etc.).
Mostly stub predicates today — table entries point at
`lagfx_op_default_handler` for the long tail.

**Symbols provided:**
- `lagfx_blit_op_table_size()`
- `lagfx_blit_op_lookup(opcode)`
- `lagfx_blit_op_table_entry(index)`
- `lagfx_blit_op_is_stub(opcode)`
- `g_blit_op_table[]` (static)

**Revival trigger:** M5 stage 30+ — macOS will issue blit-encoder
inner ops during framebuffer prep and during cursor compositing
once SkyLight is up. Defer until render path is alive; blit blocks
nothing pre-Stage 30.

**Known struct-drift on revival:**
- Identical pattern to `render_opcodes.c` — the
  `lagfx_blit_op_descriptor_t` shape is stable but each opcode
  handler needs cross-checking against current task / resource
  state.

**Forward-port preference:** Port into `src/handlers/blit/blit.c`.
Wire into `channel_compute_dispatcher.c` (blit segments share the
compute channel's command-buffer stream).

**Last-alive SHA:** `b652199`.

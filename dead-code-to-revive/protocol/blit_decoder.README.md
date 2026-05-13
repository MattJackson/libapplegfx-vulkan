# blit_decoder.c

**LOC:** 55.

**What it is:** The blit-segment inner-opcode dispatcher.
Companion to `render_decoder.c` for `encoderType == 2` (Blit)
segments. Walks the 24 blit inner ops and routes via the (separate)
blit-op table in `blit_opcodes.c`.

**Symbols provided:**
- `lagfx_blit_decoder_dispatch(protocol, segment, segment_len)`

**Revival trigger:** M5 stage 30+ — paired with `blit_opcodes.c`.
Comes online after the render path is alive; blit segments tend
to appear in mipmap-generation / surface-sync paths during
WindowServer composition.

**Known struct-drift on revival:** Same shape as the other two
decoders.

**Forward-port preference:** Inline into `src/handlers/blit/` or
extend `exec_cmdbuf.c` with a segment-type switch and delete this
TU.

**Last-alive SHA:** `b652199`.

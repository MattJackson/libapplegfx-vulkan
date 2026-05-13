# render_decoder.c

**LOC:** 65.

**What it is:** The render-segment inner-opcode dispatcher. Takes a
parsed `PGSerializerCommandSegmentHeader` whose `encoderType == 0`
(Render), walks the inner-opcode stream, and routes each 8-byte
`PGCmdHeader` to the matching entry in `g_render_op_table[]` (from
`render_opcodes.c`).

**Symbols provided:**
- `lagfx_render_decoder_dispatch(protocol, segment, segment_len)`

**Revival trigger:** M5 stage 25/30 — paired with `render_opcodes.c`
and `render_pass.c`. The live `src/handlers/compute/exec_cmdbuf.c`
walks segment headers today and acks them; once a real render
segment shows up we need this dispatcher to demultiplex inner ops.

**Known struct-drift on revival:**
- API: `lagfx_render_decoder_dispatch` is a thin adapter — easy to
  fold into a new `src/handlers/render/render.c` rather than keep
  as a standalone TU.
- The matching tests in `tests/render-opcode-draw.c` and
  `tests/render-opcode-pipeline.c` `#include
  "../src/protocol/render_decoder.h"` — that include path is now
  broken, but the tests aren't currently registered in
  `tests/meson.build` either, so nothing fails. When reviving,
  decide whether to move the header into `src/handlers/render/` or
  keep it under `src/protocol/`.

**Forward-port preference:** Inline the dispatcher into
`src/handlers/render/render.c::dispatch_segment()` and delete this
TU.

**Last-alive SHA:** `b652199`.

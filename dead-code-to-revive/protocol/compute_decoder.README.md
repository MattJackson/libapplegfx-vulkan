# compute_decoder.c

**LOC:** 55.

**What it is:** The compute-segment inner-opcode dispatcher.
Companion to `render_decoder.c` for `encoderType == 1` (Compute)
segments. Walks the 32 compute inner ops
(`setComputePipelineState`, `setBuffer`, `dispatchThreadgroups`,
etc.) and routes them via a (separate) compute-op table.

**Symbols provided:**
- `lagfx_compute_decoder_dispatch(protocol, segment, segment_len)`

**Revival trigger:** M5 stage 25/30 — once macOS issues a real
compute pipeline (e.g. through Metal Performance Shaders during
SkyLight composition). Bring up after render.

**Known struct-drift on revival:** Same shape as
`render_decoder.c` — adapter only; fold into a new
`src/handlers/compute/compute_segment.c` or extend the existing
`src/handlers/compute/exec_cmdbuf.c`.

**Forward-port preference:** Inline into `src/handlers/compute/`
and delete this TU.

**Last-alive SHA:** `b652199`.

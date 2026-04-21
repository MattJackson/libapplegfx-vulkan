# src/air2spirv — runtime AIR to SPIR-V (Phase 3.C.2 scaffold)

## Status

Phase 3.C.2 **exploratory scaffold**. Ships the first two stages
of the four-stage MSL-to-SPIR-V pipeline documented in
`the internal spec` Finding 3
and `the internal spec`. The LLVM
invocation (stage 3) and SPIR-V validation (stage 4) are
out-of-scope for this commit and left to the caller.

## Pipeline

```
    guest metallib blob
        ▼
    [1] metallib_extract    (this module)    → per-function LLVM Bitcode
        ▼
    [2] bitcode_retarget    (this module)    → LLVM Bitcode with SPIR-V triple
        ▼
    [3] llc -mtriple=spirv64-unknown-vulkan1.3 ... (external, Phase 3.C.2+)
        ▼
    [4] spirv-val (validation) + vkCreateShaderModule
```

Stages 1 and 2 are hand-rolled in C against the published MTLB
container format (`worthdoingbadly.com/metalbitcode`) and the LLVM
Bitcode wrapper format. No libLLVM linkage — that's stage 3's
concern and the call site decides whether to shell out to `llc`
or link libLLVM directly at a future phase.

## Fallback role

The runtime translator is the **miss path** from the Phase 3.C
stock-shader catalog (`src/shaders/catalog.c`). When a guest-
submitted shader hashes to a catalog miss and we lack a pre-built
SPIR-V blob for its AIR bytes, the command-buffer decoder invokes:

1. `lagfx_metallib_extract_functions(guest_buf, guest_len, ...)`
   to peel per-function LLVM Bitcode blobs out of the guest's
   metallib.
2. `lagfx_bitcode_retarget_to_spirv(bc, bc_len, &retargeted, &n)`
   to rewrite `air64-apple-macosx*` → `spir64-unknown-vulkan1.3`.
3. Hand `retargeted`/`n` to an LLVM-based SPIR-V emitter (out
   of scope for this commit; see `shader-llvm-spirv-poc-runbook.md`
   §7 for the runbook shape).
4. Feed the resulting SPIR-V to `vkCreateShaderModule`.

## Format references

Primary sources (confirmed load-bearing in the 2026-04-20 research
burst):

- [worthdoingbadly.com/metalbitcode](https://worthdoingbadly.com/metalbitcode/)
  — MTLB tagged-field container layout (header @ 0x00, FET offset
  @ 0x18, per-function NAME/TYPE/HASH/OFFT/MDSZ/VERS/ENDT tags).
- [Medium/samuliak "Reverse engineering Apple's AIR metallib format"](https://medium.com/@samuliak/reverse-engineering-apples-air-metallib-format-part-i-a0b2ca1f3e5c)
  — AIR is LLVM Bitcode with an unregistered Apple target triple.
- LLVM Bitcode wrapper magic (0x0B17C0DE) — LLVM docs
  (`BitCodeFormat.html`).
- SPIR-V target triple (`spirv64-unknown-vulkan1.3`) — LLVM 20.1
  `SPIRVUsage.html`.

Cross-reference: `the internal spec` §"Container
format" ties these specs to the library's Phase 0 corpus
(`the internal spec`, 24,500 B, 5 functions).

## Known gaps / FIXMEs

| ID                                 | Scope              | Note |
|------------------------------------|--------------------|------|
| `phase-3c2-datalayout`             | bitcode_retarget.c | Data-layout string not rewritten; runbook §6.2 plan |
| `phase-3c2-bitcode-reader`         | bitcode_retarget.c | Real bitcode record editor; uses libLLVM |
| `phase-3c2-unknown-tag`            | metallib_extract.c | Per-tag decoder for RFLT/RBUF |
| `phase-3c2-unknown-triple-format`  | bitcode_retarget.c | Extended triple patterns (non-`air64*macosx*`) |
| `phase-3c2-metallib-stages`        | metallib_extract.c | Additional stage enums (object shaders?) |

## Tests

`tests/air2spirv.c` exercises extraction + retargeting against two
fixtures:

1. **Synthesised MTLB blob** generated in-test (see
   `make_synth_mtlb` in the test source). Useful for exercising
   edge cases without touching disk.

2. **Real bytes:** `tests/fixtures/default.metallib` — a direct
   copy of `the internal spec` (24,500 bytes,
   MTLB v1.2.9, 5 shaders). Validates that our parser agrees with
   Apple's actual container layout, not just the worthdoingbadly
   writeup. Asserts:
   - MTLB magic recognised
   - Function count == 5
   - All names match the known-good set
   - Every bitcode payload begins with `0xDEC017B`
   - Every bitcode contains an `air64*-apple-macosx` triple
   - Retarget produces output containing `spir64-unknown-vulkan1.3`

The 2026-04-20 validation pass against this real fixture surfaced
and fixed four parser bugs (u16 vs u32 tag lengths, missing entry
size-prefix framing, 24-byte OFFT payload with relative bitcode
offset, stage enum values). See the commit log for the specifics.

**Validated platforms (2026-04-20):**
- Darwin arm64: 84/84 PASS (meson test)
- Alpine 3.21 musl x86_64: 84/84 PASS (via Docker)

## Build

Contributes two TUs to `lagfx_sources` via `src/air2spirv/meson.build`.
Builds on Darwin + Linux with no external dependencies (no libLLVM,
no SPIRV-Tools). The LLVM-invocation stage (`llc`) is documented
but out of the library's build chain — see `shader-llvm-spirv-poc-runbook.md`
for the runbook invoked by humans at a Mac keyboard.

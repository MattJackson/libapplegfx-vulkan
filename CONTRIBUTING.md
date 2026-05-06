# Contributing to libapplegfx-vulkan

`libapplegfx-vulkan` implements Apple's ParavirtualizedGraphics host API on Linux using Vulkan as the backend (primarily Mesa's lavapipe for CPU rendering). It replaces Apple's closed-source `libParavirtualizedGraphics.dylib` for the mos suite's use case.

The project is under active development. Contributions are welcome but expect rapid iteration.

## Build

Prerequisites:

- `meson` + `ninja`
- Vulkan loader + headers (`libvulkan`, `vulkan-headers`)
- `glslang` (for shader compilation in tests)
- A working Vulkan ICD — Mesa's lavapipe is the primary target; anything implementing Vulkan 1.3+ with `VK_EXT_shader_object` and `VK_KHR_dynamic_rendering` should work

Then:

```sh
meson setup build
meson compile -C build
```

## Testing

```sh
meson test -C build --print-errorlogs
```

All tests must pass. Some tests require a working Vulkan ICD; they auto-skip on systems without one, but CI is configured to hard-fail when the Vulkan-gated tests self-skip (to catch silent-skip regressions).

Linux-only tests (memfd coherence, Vulkan-backed end-to-end) can be validated via an Alpine 3.21 container — see `docs/` for one-liner recipes.

## Style

Match the existing `src/`:

- 4-space indent, no tabs
- K&R braces, opening brace on function definition next line, on control flow same line
- C11 (no GNU extensions beyond what Mesa relies on)
- Public C API uses the `lagfx_` prefix (`lagfx_device_new`, `lagfx_display_read_frame`, etc.)

## Naming convention

The public C API uses `lagfx_*` to avoid trademark friction with Apple's PG brand. Repository-level documentation, README keywords, and commit messages may reference "ParavirtualizedGraphics", "PGDevice", etc. for discoverability — that's intentional. Do not rename exported API symbols to `pg_*`.

## Milestones

Work is gated by the M-series milestones tracked in
[mos-docs/overview/project-status.md](https://github.com/MattJackson/mos-docs/blob/main/overview/project-status.md):

- M1 — build green + kext attaches (closed 2026-04-21)
- M2 — IOAccelerator class tree visible (closed 2026-04-21)
- M3 — `MTLCreateSystemDefaultDevice` non-nil (closed 2026-05-03)
- M4 — `CmdExecIndirect2` inner parsing + 3-level radix VA→GPA (closed 2026-04-26)
- **M5** — first visible pixel via Vulkan/lavapipe (stage 20% in progress)
- M6 — multi-display
- M7 — compute pipeline
- M8 — full Metal feature compatibility

Each stage of M5 has a single binary gate and a single test (see
[whitepaper 04](https://github.com/MattJackson/mos-docs/blob/main/whitepapers/04-m5-first-pixel.md)).
Map new work items to a milestone; if a change cross-cuts
multiple, flag it for discussion before landing.

## Reverse-engineering conventions

This project relies heavily on RE of Apple's macOS-host PVG
implementation. To keep the work clean-room and the codebase
defensible:

- **Cite the artifact, not the disassembler output.** When a
  comment or commit explains *why* a constant or layout is what it
  is, cite the binary it came from (e.g. "from
  `IOAccelerator2.kext` in macOS 15.6.1 build 24G90,
  `__DATA_CONST` vtable at `+0x8a40`"), not the decompiler's
  reconstruction. The artifact identity is reproducible; the
  decompilation is not.
- **No decompiled Apple source in the tree or in commit messages.**
  Behavioral observations, byte-exact field layouts, and dispatch
  tables read from `__DATA_CONST` are fine. Pasted decompiler-
  reconstructed C is not. When in doubt, paraphrase.
- **Primary RE notes belong in `../mos/paravirt-re/library/`.**
  Code comments may summarize and link; do not duplicate the full
  RE narrative inline.
- **Trademark caution.** Public symbols use `lagfx_*`. Repo prose
  may say "ParavirtualizedGraphics" / "PGDevice" / "PVG" for
  discoverability — that's intentional. Do not rename exported
  symbols.

## Commit discipline

- One logical change per commit.
- Commit subject: imperative, under 70 characters, prefixed by subsystem (`protocol:`, `vulkan:`, `translate:`, `air2spirv:`, `shaders:`, `docs:`, `tests:`).
- Body: explain the *why* and, where relevant, cite the re-engineering evidence (disassembly offsets, commit hashes in related mos repos) that motivates the change.
- No personal paths, no credentials, no internal domain references.
- No AI-attribution trailers (`Co-Authored-By: Claude` and the
  like). Commits go out under the contributor's own name.

## Upstream-PR staging

The `upstream-pr/` subdirectory holds code intended for future submission to external projects (Mesa, Khronos, etc.). Keep it in sync with canonical `src/` when the underlying functionality lands there — the upstream staging is a snapshot, not a divergent fork.

## License

AGPL-3.0. Network use counts as distribution. See `LICENSE`.

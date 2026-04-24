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

## Phase-based development

Work is scoped into phases following the broader mos suite's implementation plan:

- Phase 0 — protocol reverse-engineering (complete)
- Phase 1 — Linux device + decoder skeleton + Vulkan init
- Phase 2 — clear-color → first pixel on screen
- Phase 3 — Metal → Vulkan translation, shader catalog
- Phase 4 — IOSurface paravirt + VideoToolbox
- Phase 5 — performance (conditional)

Map new work items to a phase. Don't introduce features that cross-cut multiple phases without discussion.

## Commit discipline

- One logical change per commit.
- Commit subject: imperative, under 70 characters, prefixed by subsystem (`protocol:`, `vulkan:`, `translate:`, `air2spirv:`, `shaders:`, `docs:`, `tests:`).
- Body: explain the *why* and, where relevant, cite the re-engineering evidence (disassembly offsets, commit hashes in related mos repos) that motivates the change.
- No personal paths, no credentials, no internal domain references.
- `Co-Authored-By:` trailers welcome.

## Upstream-PR staging

The `upstream-pr/` subdirectory holds code intended for future submission to external projects (Mesa, Khronos, etc.). Keep it in sync with canonical `src/` when the underlying functionality lands there — the upstream staging is a snapshot, not a divergent fork.

## License

AGPL-3.0. Network use counts as distribution. See `LICENSE`.

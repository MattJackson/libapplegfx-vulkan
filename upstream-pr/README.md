# libapplegfx-vulkan — upstream-pr subset

This tree is a **submittable subset** of
[libapplegfx-vulkan](https://github.com/MattJackson/libapplegfx-vulkan),
prepared for eventual Linux-distribution packaging (Debian/Ubuntu,
Fedora, Arch, openSUSE, Alpine). It excludes development-only
scaffolding (internal protocol-spec notes, reverse-engineering
artefacts, iteration memory files, packaging-repo glue, and
milestone-tracking test runbooks) while retaining everything a
downstream packager needs to build, test, and ship the shared
library.

## What this library is

A clean-room Linux implementation of Apple's
`ParavirtualizedGraphics.framework` host-side API (the macOS-only
`PGDevice` / `PGShellCallbacks` / `PGDisplay` surface), with a
Vulkan rendering backend. It lets macOS guests under QEMU on a
Linux host route their Metal commands through Vulkan for rendering
by `lavapipe` (Mesa's software Vulkan) or any other Vulkan driver.

Architecture:

```
macOS guest (Apple kexts, unmodified)
      |   MMIO + DMA over virtual PCI
      v
QEMU (+ the apple-gfx-pci-linux companion device)
      |   C callback API
      v
libapplegfx-vulkan (this library)
      |   Vulkan API calls
      v
lavapipe / native Vulkan driver
      |   rendered pixels
      v
back up through the stack, into the display surface
```

## Our fork addition vs Apple's upstream framework

Apple ships `ParavirtualizedGraphics.framework` as a closed-source
dylib on macOS only. `libapplegfx-vulkan` is a **from-scratch
reimplementation** of the same host-side API contract on Linux,
NOT a port or derived work. In particular:

- The C API (`lagfx_*` symbols in `include/libapplegfx-vulkan.h`)
  mirrors the **shape** of Apple's Swift/Obj-C API but is authored
  independently — the `lagfx_*` prefix precisely disclaims ABI/API
  compatibility and sidesteps any trademark-adjacent naming.
- The protocol decoder in `src/protocol/` is a fresh implementation
  of the paravirt FIFO wire protocol, produced by observing the
  command stream between `AppleParavirtGPU.kext` (guest) and the
  PVG framework (host) and describing opcode semantics from
  scratch.
- The AIR-to-SPIR-V shader translator in `src/air2spirv/` uses
  publicly documented formats (MTLB container layout per
  [worthdoingbadly.com/metalbitcode](https://worthdoingbadly.com/metalbitcode/),
  LLVM Bitcode wrapper format, SPIR-V target triple) and stock
  LLVM tooling (`llc`) — no Apple IP is vendored.
- The stock shaders in `src/shaders/msl/` are original MSL sources
  authored for the paravirt display-plane role — not extracted from
  Apple's metallib.

## License

**AGPL-3.0-or-later.** See `LICENSE` (GNU AFFERO GENERAL PUBLIC
LICENSE, Version 3).

The shader translator subdirectory (`src/air2spirv/`) may carve
out to a permissively licensed standalone library in the future;
until then AGPL-3.0-or-later covers the whole tree.

## Staged contents

| Path                              | Contents |
|-----------------------------------|----------|
| `include/libapplegfx-vulkan.h`    | Public C API (installed header) |
| `src/protocol/`                   | Paravirt FIFO decoder + opcode handlers |
| `src/air2spirv/`                  | MSL/AIR to SPIR-V translator (stages 1-2) |
| `src/vulkan/`                     | Vulkan instance/device/queue/render-target setup |
| `src/shaders/`                    | Stock MSL + GLSL sources + committed SPIR-V |
| `src/display.c` / `src/display.h` | Display lifecycle + readback |
| `meson.build`                     | Top-level build |
| `LICENSE`                         | AGPL-3.0 license text |

Not staged here: `tests/`, `examples/`, `docs/`, build outputs,
CI config, per-agent memory files, internal protocol-spec notes.
These are available in the full repo for contributors.

## Build

```
meson setup build
meson compile -C build
sudo meson install -C build
```

Produces `libapplegfx-vulkan.so` plus pkg-config
`libapplegfx-vulkan.pc`. Build-time optional deps:

- **Vulkan** (`libvulkan1`, `vulkan-headers`) — optional at build
  time; falls back to a no-op stub when absent. Required at
  runtime for actual rendering.
- **glslangValidator** (`glslang-tools`) — optional. If found,
  recompiles the stock GLSL shaders to SPIR-V during the build.
  If absent, falls back to committed stub `.spv` files under
  `src/shaders/spv/`.
- **Python 3** — required. Used by the SPIR-V embedder.

## See also

- Upstream repo: https://github.com/MattJackson/libapplegfx-vulkan
- Apple's public `ParavirtualizedGraphics` docs:
  https://developer.apple.com/documentation/paravirtualizedgraphics
- Mesa lavapipe: https://docs.mesa3d.org/drivers/llvmpipe.html

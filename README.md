# libapplegfx-vulkan

**Linux clean-room reimplementation of Apple's `ParavirtualizedGraphics`
framework (the `PGDevice` / `PGShellCallbacks` / `PGDisplay` host API),
with a Vulkan rendering backend.** Enables macOS guest VMs to get
hardware-agnostic Metal acceleration on Linux hosts — routes guest
Metal commands through Vulkan, rendered by Mesa's `lavapipe` on CPU.

**Status:** Phase 1 — host-library scaffold and protocol decoder
landing. Not yet functional end-to-end.

## PG API alignment

Our C API mirrors the shape of Apple's Swift/Obj-C
`ParavirtualizedGraphics.framework`. Prefix is `lagfx_*` (not `PG*`)
to make the independent-reimplementation clear and sidestep any
trademark-adjacent naming concerns. Mapping:

| Apple `ParavirtualizedGraphics` | `libapplegfx-vulkan` |
|---|---|
| `PGDevice`                      | `lagfx_device_t` + `lagfx_device_new/free/reset` |
| `PGDeviceDescriptor`            | `lagfx_device_descriptor_t` |
| `PGShellCallbacks`              | `lagfx_shell_callbacks_t` |
| `PGDisplay`                     | `lagfx_display_t` + `lagfx_display_new/free` |
| `PGDisplayDescriptor`           | `lagfx_display_descriptor_t` |
| `PGCommandQueue` (implicit)     | handled inside the device + protocol decoder |
| MMIO in/out (shell provides)    | `lagfx_mmio_read` / `lagfx_mmio_write` |
| `mach_vm_remap` (Darwin-side)   | `lagfx_task_*` (memfd + `mmap(MAP_FIXED)` on Linux) |

If you came here from Apple's `ParavirtualizedGraphics` docs looking
for a Linux equivalent: this is it.

## What this is for

Running macOS guests under QEMU on a Linux host, with
accelerated graphics, *today has no solution*. Apple's upstream
`ParavirtualizedGraphics.framework` is macOS-host-only, and QEMU's
upstream `apple-gfx-pci.m` (which targets macOS hosts) refuses to
compile against any other platform.

`libapplegfx-vulkan` fills that gap: a clean-room reimplementation
of Apple's host-side library, using Vulkan as the rendering
pipeline so any Linux host with CPU (or, optionally, a real GPU
with a Vulkan driver) can run macOS guests with Metal working.

## Architecture

```
macOS guest (Apple kexts, unmodified)
      │   MMIO + DMA over virtual PCI
      ▼
QEMU (+ our apple-gfx-pci-linux device from mos-qemu)
      │   C callback API
      ▼
libapplegfx-vulkan  ◄─── YOU ARE HERE
      │   Vulkan API calls
      ▼
lavapipe (Mesa) — Vulkan on CPU
      │   rendered pixels
      ▼
back up through the stack, into display surface, to noVNC/user
```

## Parent project

Part of the **mos** (macOS-on-something) project stack:

- [mos-docker](https://github.com/MattJackson/mos-docker) — integrated
  product: macOS in a Docker container on Linux
- [mos-patcher](https://github.com/MattJackson/mos-patcher) — kernel
  hook framework for runtime kext patching
- [mos-qemu](https://github.com/MattJackson/mos-qemu) — our QEMU fork
  with macOS-specific patches (now also houses
  `hw/display/apple-gfx-pci-linux.c`)
- [mos-opencore](https://github.com/MattJackson/mos-opencore) — our
  OpenCore fork
- **libapplegfx-vulkan** (this repo) — host-side PVG lib

## Directory layout

```
src/           C source
  air2spirv/   AIR (Apple shader IR) → SPIR-V translator
               (subdirectory; may spin out to its own repo later)
  memory/      memfd/mmap-based task memory (Linux replacement for
               Darwin mach_vm_remap)
  protocol/    PVG wire protocol decoder
  translate/   Metal commands → Vulkan API translation
include/       Public headers (C API consumed by mos-qemu)
tests/         Unit + integration tests
docs/          Architecture notes, protocol spec, design docs
```

## Building

**Dependencies:**
- Mesa with `lavapipe` enabled (Vulkan CPU driver) — optional at
  build time during scaffold phases, required for actual rendering
- Linux kernel 5.4+ (for `memfd_create` + `mmap(MAP_FIXED)`);
  Darwin builds with a `mkstemp` fallback for dev convenience
- C11 compiler, meson ≥ 1.0, ninja, pkg-config

**Build:**
```
meson setup build
meson compile -C build
meson test -C build
```

Produces a shared library + pkg-config file; QEMU's
`apple-gfx-pci-linux` device picks it up via
`dependency('applegfx-vulkan')`.

## License

**AGPL-3.0.** See LICENSE.

The AIR → SPIR-V shader translator (`src/air2spirv/`) may carve
out to a standalone permissively-licensed repo if community
adoption interest emerges; until then, AGPL-3.0 covers the whole
tree.

## Prior art

- Apple's public [ParavirtualizedGraphics framework docs](https://developer.apple.com/documentation/paravirtualizedgraphics)
- QEMU upstream: [`hw/display/apple-gfx-pci.m` + `apple-gfx.m`](https://gitlab.com/qemu-project/qemu/-/tree/master/hw/display)
  by Phil Dennis-Jordan (Amazon), GPL-2.0+ — the macOS-host
  implementation we're porting from.
- [MoltenVK](https://github.com/KhronosGroup/MoltenVK) — precedent for Metal↔Vulkan translation (the inverse direction)
- [Mesa lavapipe](https://docs.mesa3d.org/drivers/llvmpipe.html) — the Vulkan CPU renderer we rely on

## Credits

Architected as part of the mos project by Matthew Jackson.
Paravirt GPU QEMU device template follows Phil Dennis-Jordan's
upstream QEMU work. Naming convention follows QEMU's `apple-gfx-*`
family.

## Keywords

Apple ParavirtualizedGraphics, PGDevice Linux, PGShellCallbacks,
macOS Metal on Linux, QEMU apple-gfx-pci, paravirt GPU Linux,
AppleParavirtGPU kext, PVG Linux implementation, MTLCopyAllDevices
QEMU, Metal over Vulkan, lavapipe Metal, AIR to SPIR-V.

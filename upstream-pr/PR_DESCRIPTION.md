# libapplegfx-vulkan — initial packaging drop

## Summary

`libapplegfx-vulkan` is a Linux-native, clean-room reimplementation
of Apple's `ParavirtualizedGraphics.framework` host-side API, with
a Vulkan rendering backend. It enables macOS guests running under
QEMU on a Linux host to receive hardware-agnostic Metal acceleration
— something upstream QEMU cannot do today, because its
`apple-gfx-pci` device targets the Apple-only `PG*` framework and
refuses to build on non-Darwin hosts.

This submission is the **first upstream-pr drop** of the library,
ready for evaluation by Linux distribution packagers.

## What's in this tree

A minimal, buildable, self-contained shared library plus public
header. No test suite, no examples, no development scaffolding —
those live in the upstream repo for contributors. What ships here:

- **`include/libapplegfx-vulkan.h`** — public C API consumed by
  host-side shells (notably the Linux port of QEMU's
  `apple-gfx-pci.c`).
- **`src/protocol/`** — paravirt FIFO wire-protocol decoder with
  handlers for the full M3 + display + M6/M7 opcode set.
- **`src/air2spirv/`** — MSL-to-SPIR-V translator (MTLB container
  extraction + LLVM Bitcode target-triple retargeting). Stages 3
  (LLVM `llc` invocation) and 4 (SPIR-V validation) are left to
  the caller.
- **`src/vulkan/`** — Vulkan instance + device + queue + command
  pool + render-target lifecycle.
- **`src/shaders/`** — five stock shaders (blit, clear,
  composite_over, cursor, color_fill) authored as dual MSL + GLSL
  twins, plus committed SPIR-V for both the GLSL-bypass path and
  the full Apple-pipeline path.
- **`src/display.c` / `src/display.h`** — display lifecycle +
  clear-color trigger + readback.

## Target audience

- **Distribution packagers** — Debian, Ubuntu, Fedora, Arch,
  openSUSE, Alpine maintainers who want to ship
  `libapplegfx-vulkan` as a regular system package alongside
  `qemu-system-x86_64`.
- **QEMU-on-Linux macOS-guest users** — everyone who today runs
  OSX-KVM-style stacks via CPU-only VNC because hardware Metal
  isn't available on Linux hosts.
- **The mos project** — this library is the host-side
  counterpart to the `apple-gfx-pci-linux` QEMU device that
  replaces upstream QEMU's `apple-gfx-pci.m`.

## License

AGPL-3.0-or-later. See `LICENSE`.

## Build instructions

```
meson setup build
meson compile -C build
sudo meson install -C build
```

**Dependencies (build-time):**
- C11 compiler (GCC 9+ or Clang 11+)
- Meson >= 1.0, Ninja, pkg-config
- Python 3 (SPIR-V embedder)
- Optional: `vulkan-headers` + `libvulkan1` (real rendering path)
- Optional: `glslang-tools` (recompile GLSL→SPIR-V at build time)

**Runtime dependencies:**
- `libvulkan1` + a Vulkan driver (e.g. Mesa `lavapipe` for CPU
  rendering, or any native GPU driver)
- Linux kernel 5.4+ (for `memfd_create` + `mmap(MAP_FIXED)`)

Produces:
- `/usr/lib/libapplegfx-vulkan.so.0`
- `/usr/include/libapplegfx-vulkan.h`
- `/usr/lib/pkgconfig/libapplegfx-vulkan.pc`

## Review notes

- Clean-room reimplementation — see `README.md` in this tree for
  the separation-of-concerns statement vs Apple's closed-source
  framework.
- API surface is stable in shape; minor additive extensions are
  expected as the library matures.
- Full test suite + examples live in the upstream repo at
  https://github.com/MattJackson/libapplegfx-vulkan.

# libapplegfx-vulkan

**Linux clean-room reimplementation of Apple's
`ParavirtualizedGraphics.framework` host-side library, with a
Vulkan rendering backend.** The host counterpart to the
unmodified macOS guest's PVG kexts: decodes the paravirt wire
protocol, walks the per-task radix page table, translates Metal
command streams to Vulkan, and renders through Mesa's `lavapipe`
(or any other Vulkan ICD).

This is the most technical of the [mos](https://github.com/MattJackson/mos-docs)
project's implementation repos — opcode handlers, stamp slots,
radix page tables, virtual channels all live here. The narrative
context — how this fits the stack, why Vulkan, what each
mechanism does — is in
[mos-docs](https://github.com/MattJackson/mos-docs); this README is
about *what's in the tree* and *how to consume it*.

## Status

**M5 stage 20% in progress** — pixels not yet on screen. M1–M4
green ([project status](https://github.com/MattJackson/mos-docs/blob/main/overview/project-status.md)).

| Milestone | What it gates | Status |
|---|---|---|
| M1 | Build green + kext attaches | done 2026-04-21 |
| M2 | IOAccelerator class tree visible | done 2026-04-21 |
| M3 | `MTLCreateSystemDefaultDevice()` non-nil | done 2026-05-03 |
| M4 | `CmdExecIndirect2` inner parsing + 3-level radix VA→GPA | done 2026-04-26 |
| **M5** | **First visible pixel via Vulkan/lavapipe** | stage 20% (in progress) |

Stage 20% wires the **~10 most-frequent inner Render opcodes**
(`setVertexBuffer/Bytes`, `setFragmentBuffer/Bytes`,
`setRenderPipelineState`, `drawPrimitives` + indexed/instanced
variants, `endEncoding`, `CmdExecIndirect2` recursive) plus the
matching Blit ops to lavapipe. The gate: open noVNC and see the
macOS login screen rendered by the guest's WindowServer.
Background:
[whitepaper 04 — first pixel](https://github.com/MattJackson/mos-docs/blob/main/whitepapers/04-m5-first-pixel.md).

## Where this fits

```
guest macOS (kexts unmodified)
  │  MMIO + DMA over virtual PCI
  ▼
mos-qemu  apple-gfx-pci-linux  device     ◄── dlopens libapplegfx-vulkan.so
  │  C callback API (lagfx_*)             ◄── pkg-config: libapplegfx-vulkan
  ▼
libapplegfx-vulkan   ◄── YOU ARE HERE
  │  Vulkan API
  ▼
Mesa lavapipe (or any Vulkan ICD)
  │  rendered VkImage
  ▼
back up via lagfx_display_read_frame() into QEMU's DisplaySurface
```

The library is loaded into the QEMU process at runtime; mos-qemu
links against it via `dependency('libapplegfx-vulkan')`. The
production runtime in
[mos-docker](https://github.com/MattJackson/mos-docker) bundles
`libapplegfx-vulkan.so` into its image alongside the patched QEMU.
See
[component map](https://github.com/MattJackson/mos-docs/blob/main/architecture/01-component-map.md)
for the full repo graph.

## Build

```sh
meson setup builddir
meson compile -C builddir
meson test -C builddir --print-errorlogs
```

**Dependencies:**

- C11 compiler, meson ≥ 1.0, ninja, pkg-config
- Vulkan loader + headers (`libvulkan-dev`, `vulkan-headers`) —
  optional during scaffold builds; required for actual rendering.
  When absent, meson reports `Vulkan not found — building with
  no-op Vulkan init stubs.` and the Vulkan-gated tests self-skip.
- Mesa with `lavapipe` enabled (`mesa-vulkan-swrast` on Debian/
  Alpine) — for end-to-end Vulkan rendering tests
- Linux kernel 5.4+ for `memfd_create` + `mmap(MAP_FIXED)`. Darwin
  builds with a `mkstemp` fallback for dev convenience; some
  Linux-only tests (`memory-coherence`) are built but skipped on
  Darwin
- `glslangValidator` (optional) — only needed to regenerate
  `src/shaders/spv/` from GLSL sources; committed `.spv` blobs are
  used by default

The build produces `libapplegfx-vulkan.so` (versioned) plus a
`libapplegfx-vulkan.pc` pkg-config file, installed into the prefix.
mos-qemu's `apple-gfx-pci-linux` device picks the library up via
`dependency('libapplegfx-vulkan')`.

CI (`.github/workflows/`) runs Linux + Alpine + Darwin matrices
with debug/release builds and ASan/UBSan sanitizers.

## API surface

The public C API is `include/libapplegfx-vulkan.h` — every exported
symbol is `lagfx_*`. The shape mirrors Apple's macOS-only
`ParavirtualizedGraphics.framework` (`PGDevice` /
`PGShellCallbacks` / `PGDisplay`):

| Apple API | `libapplegfx-vulkan` |
|---|---|
| `PGDevice` / `+deviceWithDescriptor:` | `lagfx_device_t` + `lagfx_device_new/free/reset` |
| `PGDeviceDescriptor` | `lagfx_device_descriptor_t` |
| `PGShellCallbacks` | `lagfx_shell_callbacks_t` |
| `PGDisplay` | `lagfx_display_t` + `lagfx_display_new/free` |
| `PGDisplayDescriptor` | `lagfx_display_descriptor_t` |
| `PGCommandQueue` (implicit) | inside the device + protocol decoder |
| MMIO read/write (host-side) | `lagfx_mmio_read` / `lagfx_mmio_write` |
| `mach_vm_remap` (Darwin) | `lagfx_task_*` (memfd + `mmap(MAP_FIXED)` + `mremap` alias on Linux) |

**Naming convention:** the public API uses the `lagfx_` prefix
(short for libapplegfx) to make the independent reimplementation
clear and sidestep Apple-trademark friction. Repo documentation,
README keywords, and commit messages may reference
"ParavirtualizedGraphics" / "PGDevice" / "PVG" for discoverability
— that's intentional. **Do not rename exported symbols to `pg_*`.**

## Code layout

```
include/libapplegfx-vulkan.h     public C API (one header)
src/
  device.c, display.c, mmio.c    lifecycle + MMIO routing
  protocol/                      PVG wire-protocol decoder
    protocol.c                   doorbell, ring drain, dispatch
    fifo.c                       per-channel FIFO drain
    opcodes.c                    outer opcode table
    ops_device.c                 CmdMapMemory / CmdUnmapMemory (vchan)
    ops_display.c                display setup, opcode 0x04 CmdDefineChildFIFO
    ops_display_vchan.c          sub-channel PGFIFO drain, cursor
    ops_cmdbuf.c                 CmdExecIndirect2 inner parsing
    ops_iosurface.c              IOSurface MMIO ring
    ops_misc.c, ops_queue.c
    render_decoder.c             encoderType=Render dispatch
    render_opcodes.c             95-entry Render opcode table (~10 wired)
    compute_decoder.c, compute_opcodes.c
    blit_decoder.c, blit_opcodes.c
    render_pass.c                render-pass state tracking
    resource_registry.c          per-task resource handles
    translate.c                  lagfx_task_translate (radix walker)
  vulkan/                        Vulkan instance/device + render targets
    instance.c, command.c, pipeline.c
    render_target.c, display_blit.c, cursor.c, iosurface.c
  translate/render_encoder.c     Metal-shape -> Vulkan command encoder
  memory/task.c                  Linux memfd + mmap(MAP_FIXED) task memory
  shaders/                       stock shader catalog (MSL src + .spv)
  air2spirv/                     AIR (Apple shader IR) -> SPIR-V translator
  common/log.h                   LAGFX_LOG / LAGFX_TRACE
tests/                           ~30 unit + integration tests
docs/                            internal architecture notes
upstream-pr/                     submittable subset for distro packaging
examples/triangle/               end-to-end example
```

The mechanisms behind these directories — *what* a stamp slot
actually is, *how* the radix walks, *why* there are three opcode
decoders — are spelled out in the mos-docs whitepapers, not here.
See "Background reading" below.

## Tests

```sh
meson test -C builddir --print-errorlogs
```

Coverage by milestone gate (full table in `tests/README.md`):

| Test | What it gates |
|---|---|
| `header-syntax-check` | Public header compiles standalone |
| `lifecycle-smoke` | Device + display lifecycle (M1) |
| `memory-task`, `memory-coherence` | Linux task memory aliasing (Phase 1.C) |
| `protocol-dispatch` | Outer opcode handler wiring |
| `m3-stamp-helpers` | Stamp ACK monotonicity + IRQ timing (M3) |
| `m4-task-translate` | 3-level radix VA→GPA translation (M4) |
| `m4-execindirect2-parser` | `CmdExecIndirect2` inner-opcode parsing (M4) |
| `m4-render-opcode-*` | Render opcode handlers wiring to Vulkan (M5) |
| `m4-blit-opcodes`, `m4-compute-opcodes` | Blit + Compute decoder paths |
| `m5-display-vchan` | Display vchan PGFIFO drain (M5 stage 10%) |
| `m5-deadlock-detect` | ABBA deadlock timing regression |
| `air2spirv`, `apple-stock-shaders` | AIR → SPIR-V end-to-end |
| `lavapipe-smoke`, `triangle-lavapipe-e2e` | Full Vulkan path (Linux only) |
| `fuzz-protocol-dispatch` | libFuzzer harness (gated `-Dfuzz=enabled`) |

Linux-only tests (`memory-coherence`, lavapipe-backed) auto-skip
on Darwin. CI hard-fails on silent skips of Vulkan-gated tests.

`tests/guest/` holds integration tests that run *inside* a macOS
guest (`MTLCreateSystemDefaultDevice`, `IOServiceMatching`,
OpenCV-driven VNC pixel detection) — driven by
`scripts/smoke-test.sh` and `scripts/regression-run.sh` against a
running mos-docker VM.

## Standing rules (any contributor / agent should know)

- **Stamps are monotonic.** Never write 0 to a stamp cell. Never
  set `stampBases[slot] = 0` to "reset" state. Background:
  [whitepaper 05](https://github.com/MattJackson/mos-docs/blob/main/whitepapers/05-stamp-slot-mechanism.md).
- **MSI vector 0 only.** All stamp / pending-bitmask interrupts
  fire on vector 0; the kext doesn't bind any other.
- **Bit number = display\_index, not chan\_id.** When setting
  `pending_displays_bitmask`, the bit position is the display
  index (0..7), not the channel id (5..12). This was M3 finding 2.
- **Validate against real upstream API headers from day 1.**
  `lagfx_*` shapes mirror Apple's `PG*` Swift/Obj-C surface;
  divergences should be deliberate and noted, not accidental.
- **Trademark caution: `lagfx_*` in symbols, "PG" /
  "ParavirtualizedGraphics" in docs.** Discoverability matters in
  prose; the binary keeps a clean prefix.

## How it's consumed

- **mos-qemu** — `apple-gfx-pci-linux` device declares
  `dependency('libapplegfx-vulkan')`. Symbols resolved at link
  time from `libapplegfx-vulkan.pc`.
- **mos-docker** — production runtime image installs
  `libapplegfx-vulkan.so` into `/usr/lib/` and the patched QEMU
  links against it. See
  [component map § "where each artifact comes from"](https://github.com/MattJackson/mos-docs/blob/main/architecture/01-component-map.md).

## Background reading

The whitepapers in mos-docs explain the mechanisms this library
implements. Read these *first* if you want to contribute:

- [Whitepaper 02 — The opcode catalog](https://github.com/MattJackson/mos-docs/blob/main/whitepapers/02-opcode-catalog.md)
  — 95 Render + 32 Compute + 24 Blit opcodes inside `CmdExecIndirect2`
- [Whitepaper 03 — Vulkan, not Metal](https://github.com/MattJackson/mos-docs/blob/main/whitepapers/03-vulkan-not-metal.md)
  — backend-choice rationale
- [Whitepaper 04 — M5 stage 20%, first pixel](https://github.com/MattJackson/mos-docs/blob/main/whitepapers/04-m5-first-pixel.md)
  — the milestone progression
- [Whitepaper 05 — The stamp slot mechanism](https://github.com/MattJackson/mos-docs/blob/main/whitepapers/05-stamp-slot-mechanism.md)
  — synchronization primitive
- [Whitepaper 06 — The radix page table](https://github.com/MattJackson/mos-docs/blob/main/whitepapers/06-radix-page-table.md)
  — task-VA → guest-PA translation
- [Whitepaper 07 — Virtual channels](https://github.com/MattJackson/mos-docs/blob/main/whitepapers/07-virtual-channels.md)
  — per-stream child rings, doorbell routing

Reference (byte layouts, opcode tables):

- [apple-gfx-pci MMIO map](https://github.com/MattJackson/mos-docs/blob/main/reference/apple-gfx-pci-mmio.md)
- [Render opcode table](https://github.com/MattJackson/mos-docs/blob/main/reference/opcode-table-render.md)
- [Compute opcode table](https://github.com/MattJackson/mos-docs/blob/main/reference/opcode-table-compute.md)
- [Blit opcode table](https://github.com/MattJackson/mos-docs/blob/main/reference/opcode-table-blit.md)

In-tree internal docs:

- `docs/lavapipe-integration.md` — Mesa lavapipe init recipe and tuning
- `docs/memory-coherence-audit.md` — the Phase 1.C `mremap` audit
  (fix landed; documented for the historical record)

## Sibling repos

| Repo | Role |
|---|---|
| [mos-docs](https://github.com/MattJackson/mos-docs) | Documentation library: overview, guides, architecture, whitepapers, reference |
| [mos](https://github.com/MattJackson/mos) | Project meta-repo: RE notes (`paravirt-re/`), milestones, memory |
| [mos-docker](https://github.com/MattJackson/mos-docker) | Production runtime container |
| [mos-qemu](https://github.com/MattJackson/mos-qemu) | QEMU 10.2.2 fork — `apple-gfx-pci-linux`, `applesmc`, `dev-hid`, `vmware_vga` patches |
| [mos-patcher](https://github.com/MattJackson/mos-patcher) | Lilu-style kext: per-instance vtable swap on Sequoia |
| [mos-opencore](https://github.com/MattJackson/mos-opencore) | OpenCore EFI image build script |

## License

**[AGPL-3.0-or-later](LICENSE).** Network use counts as
distribution. The AIR → SPIR-V shader translator (`src/air2spirv/`)
may carve out to a permissively-licensed standalone repo later if
there is community interest; until then, AGPL-3.0 covers the whole
tree.

## Reporting issues

- **Security:** [SECURITY.md](SECURITY.md) — privately via GitHub
  Security Advisories or `matthew@pq.io`.
- **Bugs / patches:** see [CONTRIBUTING.md](CONTRIBUTING.md).
- **Code of conduct:** [CODE\_OF\_CONDUCT.md](CODE_OF_CONDUCT.md).

## Prior art

- Apple's [ParavirtualizedGraphics framework docs](https://developer.apple.com/documentation/paravirtualizedgraphics)
  — the macOS-host API we mirror.
- QEMU upstream
  [`hw/display/apple-gfx-pci.m` + `apple-gfx.m`](https://gitlab.com/qemu-project/qemu/-/tree/master/hw/display)
  by Phil Dennis-Jordan (Amazon), GPL-2.0+ — the macOS-host
  implementation we're porting from. Naming convention follows
  QEMU's `apple-gfx-*` family.
- [MoltenVK](https://github.com/KhronosGroup/MoltenVK) — precedent
  for Metal↔Vulkan translation (the *inverse* direction of what
  this library does — see whitepaper 03).
- [Mesa lavapipe](https://docs.mesa3d.org/drivers/llvmpipe.html)
  — the Vulkan CPU renderer we rely on.

## Keywords

Apple ParavirtualizedGraphics, PGDevice on Linux, PGShellCallbacks,
macOS Metal on Linux, QEMU apple-gfx-pci, paravirt GPU Linux,
AppleParavirtGPU kext, PVG Linux implementation,
`MTLCopyAllDevices` under QEMU, Metal over Vulkan, lavapipe Metal,
AIR to SPIR-V.

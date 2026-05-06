# libapplegfx-vulkan — agent context

Linux clean-room reimplementation of Apple's
`ParavirtualizedGraphics.framework` host-side API, with a Vulkan
backend (Mesa lavapipe in production). Loaded into the QEMU
process at runtime by mos-qemu's `apple-gfx-pci-linux` device.

For human-facing context (what this is, build, API surface,
cross-links into mos-docs), read `README.md`.

## Status

**M5 stage 20% in progress.** M1/M2/M3/M4 closed. Stage 20% wires
the ~10 most-frequent inner Render opcodes
(`setVertexBuffer/Bytes`, `setFragmentBuffer/Bytes`,
`setRenderPipelineState`, `drawPrimitives` + indexed/instanced
variants, `endEncoding`, `CmdExecIndirect2` recursive) plus
matching Blit ops to lavapipe.

Active blocker as of last session: ABBA deadlock between
WindowServer's FB workloop command gate and the DisplayPipe's
`accel[+0x88]` IOLock. See
`../mos/paravirt-re/library/journey/deadlock-abba-analysis.md` and
the header of `src/protocol/protocol.c`.

## Standing rules (this repo)

Universal-across-mos rules: see `../mos/CLAUDE.md`. Repo-specific:

- **Stamps are monotonic.** Never write 0 to a stamp cell. Never
  `stampBases[slot] = 0`. Predicate is `>= target`, not `==`.
  Reference: `../mos/paravirt-re/library/howto/how-to-host-stamp-completion.md`.
- **MSI vector 0 only** for stamp / pending-bitmask interrupts.
- **Bit number = display\_index, not chan\_id** in
  `pending_displays_bitmask`. M3 finding 2.
- **Validate against real upstream API headers from day 1** —
  `lagfx_*` shapes mirror Apple's `PG*` Swift/Obj-C surface;
  divergences should be deliberate, never accidental.
- **`lagfx_*` in symbols, "PG" / "ParavirtualizedGraphics" in
  prose.** Do not rename exported symbols to `pg_*`.
- **No `Co-Authored-By: Claude` trailers.** Strip on sight.

## Code layout (where things live)

```
include/libapplegfx-vulkan.h    public C API (lagfx_* only)
src/protocol/                   PVG wire-protocol decoder
  protocol.c                    doorbell, ring drain, dispatch
  fifo.c                        per-channel FIFO drain
  opcodes.c                     outer opcode table
  ops_cmdbuf.c                  CmdExecIndirect2 inner parsing
  ops_display.c, ops_display_vchan.c   display vchan + cursor
  ops_device.c                  CmdMapMemory / CmdUnmapMemory
  render_decoder.c, render_opcodes.c    ~10 of 95 wired (M5 stage 20%)
  compute_decoder.c, compute_opcodes.c
  blit_decoder.c, blit_opcodes.c
  translate.c                   lagfx_task_translate (radix walker)
  resource_registry.c           per-task resource handles
src/vulkan/                     instance, command pool, render targets
src/translate/render_encoder.c  Metal-shape -> Vulkan command encoder
src/memory/task.c               memfd + mmap(MAP_FIXED) + mremap alias
src/air2spirv/                  AIR (Apple shader IR) -> SPIR-V
src/shaders/                    stock shader catalog (MSL + .spv)
tests/                          unit + integration; tests/guest/ runs in-VM
scripts/smoke-test.sh           WindowServer crash-report regression gate
scripts/m5-progress.sh          10-level M5 progress harness (10/20/.../100)
scripts/regression-run.sh       full local CI (meson + smoke + VNC)
```

QEMU-side PCI device is in `../qemu-mos15/` (public name: `mos-qemu`).
Primary RE notes: `../mos/paravirt-re/library/`.

## Build + test loop

```sh
meson setup builddir
meson compile -C builddir
meson test -C builddir --print-errorlogs
```

Vulkan is detected at configure time. When absent, meson warns
"Vulkan not found — building with no-op Vulkan init stubs." and
gates compile out the real init path; Vulkan tests self-skip. CI
hard-fails on silent skips so prod regressions don't sneak through.

Production deploy (after a green local build):

```sh
# from /Users/mjackson/Developer/mos-docker, rebuild image with current source
ssh docker 'cd $HOME/mos-docker && sudo docker compose build --no-cache macos'
ssh docker 'cd $HOME/mos-docker && sudo docker compose up -d macos'
export SMOKE_GUEST=matthew@10.1.7.20
./scripts/smoke-test.sh
```

## Cross-links

- **Whitepapers (mechanisms):** `../mos-docs/whitepapers/{02,03,04,05,06,07}.md`
  — opcodes, Vulkan choice, M5 progression, stamps, radix, vchans.
- **Reference (byte layouts):** `../mos-docs/reference/{apple-gfx-pci-mmio,opcode-table-render,opcode-table-compute,opcode-table-blit}.md`.
- **Primary RE notes:** `../mos/paravirt-re/library/{state-machines,classes,howto,journey}/`.
- **Project memory:** `../mos/memory/MEMORY.md`.

When in doubt, read `mos-docs/whitepapers/`.

## TODO comments in code

In-source TODOs typically point at items tracked in
`../mos/paravirt-re/library/MASTER-RE-TODO.md` or one of the
opcode-table TSVs. Leave them alone unless you're closing the
referenced item.

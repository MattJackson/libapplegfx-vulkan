# Changelog

All notable changes to `libapplegfx-vulkan` will be documented in this
file.

The format is based on [Keep a Changelog 1.1.0](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

> **Status:** pre-1.0, active development. The public C API
> (`lagfx_*`) is **not yet stable** — symbol names, struct layouts,
> and callback signatures may change without notice until a tagged
> `1.0.0` release. Consumers (currently only `mos-qemu`'s
> `apple-gfx-pci-linux` device) should pin to a specific commit
> rather than a version range.

## v0.1.0 — 2026-07-19

First ABI-stable release. Public API frozen to the 20 exported
`lagfx_*` symbols consumed by the QEMU `apple-gfx-pci` device (device
lifecycle, MMIO entry, task memory, display/read_frame, vblank tick,
shader catalog, version introspection). Default-hidden symbol
visibility with `LAGFX_EXPORT` annotations; `applegfx/` public header
subdir; pkg-config `libapplegfx-vulkan.pc`; soname `.so.0`. Semver
from here on.

## [Unreleased]

### Added
- **M4 closure (2026-04-26):** WindowServer no longer crashes in
  `MetalShader::CopyPipelineState`. Crash signature shifted to
  user-space `CA::OGL::MetalContext::create_pipeline_state` with
  `"rasterSampleCount (1) is not supported by device"` — that's M5
  territory (Metal feature reply correctness), not M4 regression.
- `scripts/smoke-test.sh` — VM boot + WindowServer crash-report
  regression gate. Classifies crashes as M4-regression / M5-tolerated /
  unexpected. Filters by the report's own `captureTime` field
  (file mtime is unreliable — ReportCrash flushes batched reports
  later).
- `scripts/m5-progress.sh` — automated harness for the 10-level M5
  progress scale (`mos/memory/project_m5_progress_scale_2026_04_26.md`).
  Levels 10–70 fully automated; 80 (perf), 90 (1hr stability),
  100 (visual diff) are SKIP-manual. Captures via QEMU monitor
  `screendump` (works regardless of guest login state).
- `tests/fuzz-protocol-dispatch.c` — libFuzzer harness for the
  protocol dispatcher; gated `-Dfuzz=enabled`. Verified on Alpine
  clang-19: 2.1M iterations/6s green.
- `src/protocol/translate.c` — `lagfx_task_translate` extracted from
  `protocol.c` into its own TU.
- Three log levels (`LAGFX_LOG_LEVEL=warn|info|trace`) plus the
  `LAGFX_TRACE` macro. Default level = `warn` (prod-safe). Per-cmd /
  per-translate / per-MMIO call sites converted to TRACE.

### Changed
- `LAGFX_OP_ROOT_CHANNEL_INVALIDATE` → `LAGFX_OP_MAP_MEMORY_IMMEDIATE`
  (the old name was wrong on two counts: not on root channel, doesn't
  invalidate). Per `paravirt-re/library/journey/opcodes-0x35-0x36-0x39.md`.
- Per-channel ring drain now walks `page0[off>>12]` per chunk and
  stitches the result, instead of using `page0[0]` as a single base
  GPA + adding `cur_rp`. The latter only worked when the kernel
  allocator gave physically-contiguous pages; on a fragmented heap
  cmds past offset 0xfff read garbage. Drop the `+0x1000` fallback.

### Fixed
- Per-channel ring `cmd_len` bounds check no longer overflows when
  the kext writes a sentinel `0xFFFFFFFF` length (e.g. ch=1 idle
  doorbell with empty data). Compare against available span instead
  of `cur_rp + cmd_len`.
- Segment walker no longer rejects `host_gpu_addr == 0u` — VA=0 is
  a legitimate task-VA; the radix-walker fallback already handles
  unmapped cases.
- `lagfx_op_define_host_task` overwrites `root_page_pfn` cleanly
  on slot reuse. Documented that the kext recycles taskIDs across
  many task instances; the simple overwrite is correct because the
  kext has freed the prior radix pages.

### Removed
- `lagfx_task_mapping_t` / `LAGFX_MAX_TASK_MAPPINGS` /
  `lagfx_task_entry_t::mappings[]`. Was populated from 0x39
  CmdMapMemoryImmediate trailers, scanned in `lagfx_task_translate`,
  but never authoritative — translation always fell through to the
  radix walk. Pure ballast; can be re-added if multi-PA scatter
  prefix RE produces per-segment GPAs to store.

---

## [0.1.0-preview] — rolling preview

First pre-release tag is not yet cut; everything below is what has
landed on `main` so far. Rough grouping by phase (per the
implementation plan in `docs/`).

### Phase 1.A — host-library scaffold
- **1.A.1:** repo skeleton, AGPL-3.0 (relicensed MIT 2026-07-19), meson build, public C API
  header, device/display/MMIO no-op objects, `lagfx_task_*` memory
  API stubs.
- **1.A.2:** PVG protocol decoder + dispatcher. P0 opcode handlers
  landed (`CmdNOP`, `CmdDebug`, `CmdGetDeviceInfo`, `CmdDefineTask2`,
  `CmdDeleteTask`, `CmdDefineChildFIFO`, `CmdDeleteChildFIFO`,
  `CmdSynchronizeResources`). P1 handlers landed (`CmdMapMemory2`,
  `CmdUnmapMemory`, `CmdExecIndirect2`). Fixed command-header size
  (12 bytes, not 16) and corrected MMIO 0x101c semantics
  (`_rootPageNumber`, not doorbell).
- Pkg-config name standardized to `libapplegfx-vulkan` (M1 fix).
- `gpu_cores` descriptor field plumbed into `LP_NUM_THREADS` for
  lavapipe worker-pool sizing.

### Phase 1.B — Vulkan init
- Vulkan instance + physical-device selection + logical device +
  queue init (no rendering yet).
- **1.B.2:** Vulkan command pool + empty-submit smoke test.
- Integration: VK empty-submit wired into decoder
  `CmdSync`/`ExecIndirect` completion path.

### Phase 1.C — Linux task memory
- `memfd_create` + `mmap(MAP_FIXED)`-based task memory replaces
  Darwin's `mach_vm_remap`. Fix: `lagfx_task_map_host_memory` aliases
  host pointer via `mremap` (was copy-on-map).
- `task_map_via_copy` SIGSEGV fix on read-only fallback path.

### Phase 2 — first pixel
- **2.A:** display opcode handlers (`DisplayAck`,
  `DisplaySwapMapping`, `DisplayTransaction3`) — first-pixel plumbing.
- **2.B:** Vulkan clear-color render target + readback — the
  first-pixel path closes end-to-end.

### Phase 3 — Metal → Vulkan translation (in progress)
- **3.A partial:** `CmdExecIndirect2` inner-opcode dispatch scaffold.
- **3.A:** Metal-to-Vulkan render command encoder skeleton.
- **3.C scaffold:** 5-shader catalog + runtime lookup stub; real MSL
  + GLSL sources for the stock shaders.
- **3.C.2:** runtime AIR→SPIR-V exploratory scaffold.

### M3/M4/M5/M6 bring-up (iterative)
- **M3 plumbing:** ring-geometry setters, real FIFO drain, DMA
  writeback, `ring_base_gpa` fix, `actual_count` writeback,
  `CmdGetDeviceInfo2` zero-pair fallback, 11 extended opcodes from
  A2 kext disasm (§13.5), `0x30/0x33/0x38/0x3a` handlers, modern
  capability path via BAR0+0x122c = 9, stamp-page writeback
  experiments.
- **M4:** scanout-VA writeback for `CmdDisplayTransaction3`, guest
  runbook for metal-clear-color.
- **M5 (air2spirv):** **library end-to-end — Apple AIR → lavapipe
  red pixel** (commit `96cff5f`). MTLB parser fixed to match Apple's
  real v1.2.9 layout; real-bytes metallib test + fixture; strip
  Kernel-dialect decorations (`FPFastMathMode`, `MaxByteOffset`,
  Kernel-only `FuncParamAttr` values, and on `OpMemberDecorate`).
  Stock shaders now ship via the Apple-AIR pipeline.
- **M6 gap-closure:** cursor + shared-page handlers
  (`0x19/0x1a/0x27-0x29`); pending-stamp queue so batched
  completions aren't lost; atomic read-and-clear for stamp cells
  `0x1014/0x1018`; `last_completed_stamp` returned at `0x102c`;
  unknown-opcode hex dump.

### Tooling / CI
- Linux + Alpine + Darwin meson CI matrix; sanitizers (ASan/UBSan).
- Debug/release build-type matrix for `ubuntu-24.04`.
- Hard-fail Vulkan detection + assert `vulkan-*` tests actually run.
- PR + issue templates, CODEOWNERS.
- Trace-replay test registered under meson.

---

[Unreleased]: https://github.com/MattJackson/libapplegfx-vulkan/compare/HEAD...HEAD
[0.1.0-preview]: https://github.com/MattJackson/libapplegfx-vulkan/commits/main

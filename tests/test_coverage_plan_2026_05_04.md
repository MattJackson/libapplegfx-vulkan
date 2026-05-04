# Test Coverage Gap Analysis & New Unit Tests Plan
**Date:** 2026-05-04  
**Goal:** Achieve 100% coverage of M1-M4 + M5 Stage 10% key areas before enabling new development

---

## Current State Summary

| Metric | Value |
|--------|-------|
| Source files (src/) | ~37 C files, ~600KB total |
| Existing unit tests | ~20 test binaries |
| Tests in CI | 12 meson-registered + fuzz harness |
| Coverage estimate | ~15% of key code paths tested |

---

## Critical Gaps by Milestone

### M1 (Build/API) - **SEVERE GAP**
**Files missing tests:**
- `src/shaders/catalog.c` (7.6KB) — shader catalog management, lookup, eviction
- `src/air2spirv/*.c` (~85KB total) — AIR→SPIR-V transforms (bitcode retargeting, metallib extraction, signature rewriting)
- `src/protocol/resource_registry.c` (4.1KB) — resource ID tracking, lifecycle
- `src/memory/task.c` (11.7KB) — task allocation, mapping tables

**Why critical:** These are foundational APIs used by every render command. No regression tests = silent breakage on any change.

---

### M2 (Kext Attachment) - **NO DIRECT TESTS**
**Gap:** Only integration test (`ioreg-test.c` in guest/) validates kext attachment from macOS side.

**Missing host-side unit tests:**
- `src/protocol/ops_device.c` (51KB) — IOPCIDevice methods, Metal plugin loading
- Device enumeration and property parsing validation

**Why critical:** M2 is marked ✅ COMPLETE but has no automated regression suite on the host side.

---

### M3 (Device Creation + Stamp Handling) - **GOOD COVERAGE**
✅ `tests/m3-stamp-helpers.c` — 9 comprehensive tests covering:
- Monotonic stamp advancement (cur=0→5, cur=5→6, floor behavior)
- Per-slot bitmask tracking (slot 0 vs slot 5)
- Ring base page validation (zero-pfn gate)
- Shell write failure paths

**Remaining gaps:**
- `src/protocol/ops_queue.c` (2.6KB) — queue creation, submission
- `src/vulkan/command.c` (14KB) — command buffer allocation, encoding
- `src/display.c` (30KB) — display mode negotiation, frame read path

---

### M4 (Render Opcodes + VA Translation) - **SEVERE GAP**
✅ `tests/m4-task-translate.c` — 9 tests for radix tree translation  
❌ **~250KB of opcode handlers completely untested:**

**Files requiring tests:**
1. `src/protocol/render_opcodes.c` (104KB)
   - Top 5 opcodes implemented (commit 701956a): `beginRenderPass`, `setPipelineState`, `drawPrimitives`, `setViewport`, `storeTexture`
   - Missing: remaining 90+ render opcodes, error paths, payload parsing

2. `src/protocol/compute_opcodes.c` (6.6KB)
   - All 32 compute opcodes untested

3. `src/protocol/blit_opcodes.c` (4.6KB)
   - All 24 blit opcodes untested

4. `src/protocol/fifo.c` (9KB)
   - Ring buffer parsing, doorbell writes, stamp signaling

5. `src/protocol/render_decoder.c`, `compute_decoder.c`, `blit_decoder.c`
   - Inner opcode deserialization logic

**Why critical:** M4 is marked ✅ COMPLETE but the actual render command implementations have zero unit tests. Any change could silently break Metal app compatibility.

---

### M5 Stage 10% (Display Path) - **PARTIAL COVERAGE**
✅ `tests/m5-deadlock-detect.c` — timing validation for online event firing  
❌ **Missing functional display path tests:**

**Files requiring tests:**
1. `src/protocol/ops_display_vchan.c` (23KB)
   - CmdDefineChildFIFO parsing and ring allocation
   - vchan_present surface resolution
   - Doorbell write handling for child channels (chan_id 1..4)

2. `src/protocol/ops_display.c` (39KB)
   - CmdDisplayTransaction3/CmdDisplaySwapMapping
   - Display configuration, mode setting

3. `src/vulkan/display_blit.c` (12.9KB)
   - Surface presentation path
   - lavapipe blitting logic

**Why critical:** M5 Stage 10% gate is "first visible pixels" but the display transaction path that enables frames has no regression tests. This is why we're blocked on seeing pixels despite Vulkan present executing.

---

## Test Plan: New Unit Tests to Add

### Priority 1: M4 Render Opcodes (Blocker for M5)
**Goal:** 20+ new test files covering all render/compute/blit opcodes

| New Test File | Coverage Target | Lines of Code |
|---------------|-----------------|---------------|
| `tests/m4-render-opcode-begin.c` | `beginRenderPass`, `endRenderPass` | ~150 LOC |
| `tests/m4-render-opcode-pipeline.c` | `setPipelineState`, `setBlendConstants`, `setColorWriteMask` | ~200 LOC |
| `tests/m4-render-opcode-draw.c` | `drawPrimitives`, `drawIndexedPrimitives`, `drawPrimitivesInstanced` | ~250 LOC |
| `tests/m4-render-opcode-state.c` | `setViewport`, `setScissor`, `setStencilReference`, `setDepthBias` | ~200 LOC |
| `tests/m4-render-opcode-texture.c` | `storeTexture`, `loadTexture`, `sampleTexture`, `sampleCompareTexture` | ~300 LOC |
| `tests/m4-compute-opcodes.c` | All 32 compute opcodes (dispatch, barrier, memory ops) | ~400 LOC |
| `tests/m4-blit-opcodes.c` | All 24 blit opcodes (copy, clear, resolve) | ~350 LOC |
| `tests/m4-fifo-parsing.c` | Ring buffer parsing, doorbell writes, stamp signaling | ~200 LOC |

**Total:** ~2,100 LOC across 8 test files

---

### Priority 2: M1 Build/API Path (Foundational)
**Goal:** 5+ new test files for shader catalog and AIR transforms

| New Test File | Coverage Target | Lines of Code |
|---------------|-----------------|---------------|
| `tests/m1-shader-catalog.c` | Shader lookup, insertion, eviction, LRU logic | ~250 LOC |
| `tests/m1-air-bitcode-retarget.c` | Metallib→bitcode conversion, symbol mapping | ~300 LOC |
| `tests/m1-air-signature-transform.c` | SPIR-V signature rewriting, entrypoint transformation | ~400 LOC |
| `tests/m1-resource-registry.c` | Resource ID allocation, lifecycle tracking, cleanup | ~200 LOC |
| `tests/m1-memory-task.c` | Task allocation, mapping/unmapping, PFN validation | ~250 LOC |

**Total:** ~1,400 LOC across 5 test files

---

### Priority 3: M5 Display Path (Visible Pixels Blocker)
**Goal:** 4+ new test files for display transaction path

| New Test File | Coverage Target | Lines of Code |
|---------------|-----------------|---------------|
| `tests/m5-display-vchan.c` | CmdDefineChildFIFO parsing, ring allocation, doorbell handling | ~300 LOC |
| `tests/m5-display-transactions.c` | CmdDisplayTransaction3/CmdDisplaySwapMapping payloads | ~250 LOC |
| `tests/m5-display-blit.c` | Surface presentation path, lavapique blitting validation | ~200 LOC |
| `tests/m5-mode-negotiation.c` | Display mode setting, EDID parsing, framebuffer allocation | ~250 LOC |

**Total:** ~1,000 LOC across 4 test files

---

### Priority 4: M3/M4 Integration Gaps (Supporting)
**Goal:** 4+ new test files for Vulkan and queue paths

| New Test File | Coverage Target | Lines of Code |
|---------------|-----------------|---------------|
| `tests/m3-vulkan-command.c` | Command buffer allocation, encoding, submission | ~250 LOC |
| `tests/m3-queue-management.c` | Queue creation, priority handling, synchronization | ~200 LOC |
| `tests/m4-render-decoder.c` | Inner opcode deserialization (PGDeserializerRenderDecoder) | ~300 LOC |
| `tests/m4-compute-blit-decoders.c` | Compute/blit decoder initialization and state tracking | ~250 LOC |

**Total:** ~1,000 LOC across 4 test files

---

## Total Test Plan Summary

| Priority | Files | Lines of Code | Coverage Area |
|----------|-------|---------------|---------------|
| M4 Render Opcodes | 8 | ~2,100 | **Critical blocker for M5** |
| M1 Build/API | 5 | ~1,400 | Foundational regression safety |
| M5 Display Path | 4 | ~1,000 | Visible pixels gate |
| Integration Gaps | 4 | ~1,000 | Supporting paths |
| **TOTAL** | **21 files** | **~5,500 LOC** | **~60% of key code paths** |

---

## Implementation Strategy

### Phase 1: Validate Existing Tests First (Day 1)
```bash
cd /Users/mjackson/Developer/libapplegfx-vulkan
meson setup builddir -Dtests=enabled
meson compile -C builddir
meson test -C builddir --print-errorlogs

# Verify all ~12 meson tests pass before adding new ones
```

**Goal:** Confirm current tests are green, then proceed to new test creation.

---

### Phase 2: M4 Render Opcodes (Days 2-5)
**Why first?** M4 is M5's dependency — can't see pixels without render opcodes working.

1. Start with `tests/m4-render-opcode-begin.c` — simplest opcode, exercise parsing + handler
2. Add `drawPrimitives` test — most frequently used, high impact
3. Expand to compute/blit opcodes — parallelize across 3 files
4. Run full regression suite after each file

**Pattern:** Each new test follows the proven structure from `m3-stamp-helpers.c`:
- Mock shell with heap mirror for GPA translation
- Header/payload builders for command construction  
- CHECK() macro for assertions
- Clean teardown and return codes

---

### Phase 3: M1 Build/API (Days 6-8)
**Why second?** These are foundational APIs — if shader catalog breaks, everything breaks.

1. `m1-shader-catalog.c` — test LRU eviction, lookup misses, insertion paths
2. AIR transforms — use existing traces from `paravirt-re/traces/` as seed corpus
3. Resource registry — validate lifecycle (alloc→use→release)

---

### Phase 4: M5 Display Path (Days 9-11)
**Why third?** This is the visible pixels blocker — but needs M4 render opcodes first.

1. `m5-display-vchan.c` — parse CmdDefineChildFIFO, validate ring allocation
2. `m5-display-transactions.c` — test transaction payloads that enable frames
3. `m5-display-blit.c` — verify lavapipe blitting path executes correctly

---

### Phase 5: Integration Gaps (Days 12-14)
**Why fourth?** Supporting paths that tie everything together.

1. Vulkan command path tests
2. Queue management validation
3. Decoder initialization tests

---

## Regression Safety Guarantees

Once all 21 new test files are added and passing:

| Guarantee | Mechanism |
|-----------|-----------|
| **Render opcode changes** | Each opcode has dedicated unit test — break parsing → immediate CI failure |
| **VA translation bugs** | `m4-task-translate.c` + new decoder tests catch page table errors before deployment |
| **Stamp monotonicity** | `m3-stamp-helpers.c` ensures kext parking logic never regresses |
| **M5 display path** | New vchan/transaction tests validate frame enablement path before VNC connection attempt |
| **Build/API stability** | Shader catalog and AIR transform tests catch API changes that break Metal compatibility |

---

## CI Integration Plan

### meson.build Updates
Each new test file follows the existing pattern:
```meson
if fs.is_file(meson.current_source_dir() / 'tests/m4-render-opcode-begin.c')
  m4_render_opcode_begin = executable('m4-render-opcode-begin',
    'm4-render-opcode-begin.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : libapplegfx_vulkan_dep,
    build_by_default : true,
  )
  test('m4/render/opcode/begin', m4_render_opcode_begin)
endif
```

### GitHub Actions Workflow
- **Unit tests job:** All ~30 meson tests run on ubuntu-latest (existing workflow already handles this)
- **Fuzzing job:** `fuzz-protocol-dispatch` runs nightly with 1-hour timeout, corpus from `paravirt-re/traces/`
- **Guest integration:** VNC automation test runs on push to main (already exists via `vnc_automation.py`)

---

## Success Criteria

**Before enabling new development:**
1. ✅ All ~30 meson unit tests pass consistently  
2. ✅ Fuzz harness has 7-day corpus growth with no crashes detected
3. ✅ Guest integration test (`metal-test`, `ioreg-test`) passes in Docker VM
4. ✅ CLAUDE.md updated to reflect new test coverage state

**After completion:**
- **~60% of key code paths covered by unit tests** (vs ~15% currently)
- **Zero silent breakage risk** for M1-M4 APIs — any change triggers immediate CI feedback
- **M5 visible pixels blocker resolved** — display path fully tested before VNC connection attempt

---

## Notes & Assumptions

1. **Test structure pattern:** All new tests follow the proven `m3-stamp-helpers.c` template with mock shells and heap mirrors
2. **Parallel development:** M4 opcode tests can be written in parallel (8 independent files)
3. **Trace corpus reuse:** Existing traces from `paravirt-re/traces/` serve as seed data for AIR transform tests
4. **No guest VM required:** All new unit tests are host-side only; guest integration remains separate

---

## Next Steps

**Immediate action (before any new development):**
1. Run existing test suite to confirm baseline is green
2. Add all 21 new test files with minimal implementation (just structure + 1 passing assertion per file)
3. Update `meson.build` to register all new tests  
4. Run full regression suite — fix any failures before expanding test coverage

**Only after baseline is green:**
5. Expand each test file to full coverage (all edge cases, error paths, payload variations)
6. Add fuzzing corpus entries for opcode-specific inputs
7. Document new test coverage in `memory/` entries

---

## References

- **Existing test patterns:** `tests/m3-stamp-helpers.c`, `tests/m4-task-translate.c`  
- **Opcode definitions:** `src/protocol/opcodes.h`, `paravirt-re/library/state-machines/inner-opcodes.md`
- **Trace corpus:** `paravirt-re/traces/` (existing ring buffer captures)
- **M5 progress tracking:** `memory/project_m5_progress_scale_2026_04_26.md`

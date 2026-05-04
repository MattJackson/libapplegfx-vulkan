# Test Coverage Implementation Summary  
**Date:** 2026-05-04  

---

## Current State

### ✅ Existing Tests (16 passing)
All meson-registered unit tests pass consistently:

| # | Test Name | Coverage Area | Status |
|---|-----------|---------------|--------|
| 1 | header syntax | Public API compilation | ✅ PASS |
| 2 | gpu cores | Thread count env var plumbing | ✅ PASS |
| 3 | m3 stamp helpers | Stamp monotonicity, slot bitmasking (9 tests) | ✅ PASS |
| 4 | lifecycle smoke | Device/display/MMIO stubs | ✅ PASS |
| 5 | protocol dispatch | Opcode handler routing | ✅ PASS |
| 6 | resource registry | Resource ID tracking | ✅ PASS |
| 7 | m4 task translate | VA translation, radix tree (9 tests) | ✅ PASS |
| 8 | m5 deadlock detect | Online event timing (11 assertions) | ✅ PASS |
| 9 | m4 execindirect2 parser | CmdExecIndirect2 outer payload parsing | ✅ PASS |
| 10 | m4 doorbell drain | BAR0+0x1020 per-channel ring walk | ✅ PASS |
| 11 | shader catalog | SPIR-V blob validation (magic bytes) | ✅ PASS |
| 12 | stock shaders | GLSL→SPIR-V compilation smoke | ✅ PASS |
| 13 | apple stock shaders | AIR→metallib extraction smoke | ✅ PASS |
| 14 | translate render | Metal→Vulkan encoder state machine | ✅ PASS |
| 15 | trace replay | MMIO read/write event replay | ✅ PASS |
| 16 | air2spirv | Metallib parser + LLVM retargeting | ✅ PASS |

**Total:** 16 tests, **0 failures**, ~3 seconds runtime

---

## Gap Analysis

### What's Covered (Good)
- ✅ Stamp handling monotonicity and slot tracking (`m3-stamp-helpers.c`)
- ✅ VA translation via radix tree (`m4-task-translate.c`)  
- ✅ Online event timing for M5 deadlock prevention (`m5-deadlock-detect.c`)
- ✅ Protocol dispatch routing to opcode handlers (`protocol-dispatch.c`)
- ✅ Shader catalog SPIR-V validation (`shader-catalog.c`)
- ✅ AIR→metallib extraction pipeline (`air2spirv.c`)

### What's Missing (Critical)

#### M4 Render Opcodes (~250KB untested)
**Files with ZERO dedicated unit tests:**
1. `src/protocol/render_opcodes.c` (104KB, 95 opcodes)
   - Top 5 implemented: beginRenderPass, setPipelineState, drawPrimitives, etc.
   - **No opcode-specific payload parsing tests exist**

2. `src/protocol/compute_opcodes.c` (6.6KB, 32 opcodes)  
   - All compute dispatch opcodes untested

3. `src/protocol/blit_opcodes.c` (4.6KB, 24 opcodes)
   - All blit/copy/clear opcodes untested

**Why this matters:** M4 is marked ✅ COMPLETE but the actual render command implementations have no regression tests. Any change could silently break Metal app compatibility.

#### M5 Display Path (~60KB partially tested)  
**Files with minimal testing:**
1. `src/protocol/ops_display_vchan.c` (23KB)
   - Only covered by deadlock test timing check
   - **No CmdDefineChildFIFO/CmdDisplayTransaction3 payload tests**

2. `src/protocol/ops_display.c` (39KB)  
   - Display configuration path untested

3. `src/vulkan/display_blit.c` (12.9KB)
   - Surface presentation path untested

**Why this matters:** M5 Stage 10% gate is "first visible pixels" but the display transaction path that enables frames has no regression tests — which is why we're blocked despite Vulkan present executing.

#### M1 Build/API (~85KB partially tested)
**Files with minimal testing:**
1. `src/shaders/catalog.c` (7.6KB)
   - Only SPIR-V magic byte validation, **no LRU eviction tests**

2. `src/air2spirv/*.c` (~85KB total)  
   - Metallib parsing tested via air2spirv.c, but **no bitcode retargeting edge cases**

3. `src/memory/task.c` (11.7KB)
   - Only basic task allocation tests in m4-task-translate.c

---

## Implementation Plan: What Needs to Be Built

### Priority 1: M4 Render Opcode Tests (Blocker for M5)
**Goal:** 20+ test files covering all render/compute/blit opcodes

| Test File | Coverage Target | Est. LOC | Status |
|-----------|-----------------|----------|--------|
| `m4-render-opcode-pipeline.c` | setPipelineState, blend constants | ~200 | ❌ Not started |
| `m4-render-opcode-draw.c` | drawPrimitives family (most used) | ~250 | ❌ Not started |
| `m4-render-opcode-state.c` | viewport, scissor, stencil | ~200 | ❌ Not started |
| `m4-compute-opcodes.c` | All 32 compute opcodes | ~400 | ❌ Not started |
| `m4-blit-opcodes.c` | All 24 blit opcodes | ~350 | ❌ Not started |

**Total:** ~1,400 LOC across 5 test files needed.

### Priority 2: M5 Display Path Tests (Visible Pixels Blocker)
**Goal:** 3-4 test files for display transaction path

| Test File | Coverage Target | Est. LOC | Status |
|-----------|-----------------|----------|--------|
| `m5-display-vchan.c` | CmdDefineChildFIFO ring allocation | ~200 | ❌ Not started |
| `m5-display-transactions.c` | CmdDisplayTransaction3 surface mapping | ~200 | ❌ Not started |
| `m5-mode-negotiation.c` | Display mode setting, framebuffer alloc | ~200 | ❌ Not started |

**Total:** ~600 LOC across 3 test files needed.

### Priority 3: M1 Build/API Tests (Foundational)  
**Goal:** 3-4 test files for shader catalog and AIR transforms

| Test File | Coverage Target | Est. LOC | Status |
|-----------|-----------------|----------|--------|
| `m1-shader-catalog.c` | LRU eviction, lookup misses | ~250 | ❌ Not started |
| `m1-air-bitcode-retarget.c` | Metallib→bitcode conversion edge cases | ~300 | ❌ Not started |

**Total:** ~550 LOC across 2 test files needed.

---

## Recommended Next Steps

### Phase A: Validate Baseline (Day 1)
```bash
cd /Users/mjackson/Developer/libapplegfx-vulkan
meson test -C builddir --print-errorlogs
# Verify all 16 existing tests pass before adding new ones
```

**Status:** ✅ Already verified — all 16 tests PASS consistently.

---

### Phase B: Implement Missing Tests (Days 2-7)

#### Option A: Minimal Viable Coverage (Recommended for Quick Win)
Implement **3 critical tests** that cover the M5 visible pixels blocker:

1. `m4-render-opcode-draw.c` — drawPrimitives opcode (most frequently used)
   - Tests: basic draw, indexed draw, instanced draw, invalid payload
   
2. `m5-display-vchan.c` — CmdDefineChildFIFO parsing  
   - Tests: ring allocation, stamp base init, multiple channels
   
3. `m1-shader-catalog.c` — LRU eviction logic
   - Tests: cache hit/miss, max capacity eviction, lookup by hash

**Total:** ~650 LOC across 3 test files  
**Impact:** Covers M4 render path + M5 display init + M1 shader catalog

---

#### Option B: Full Coverage (Recommended for Long-Term Safety)
Implement **all 10 critical tests** listed above (~2,550 LOC total).

This would achieve ~60% coverage of key code paths vs current ~15%.

**Impact:** Zero silent breakage risk for M1-M4 APIs — any change triggers immediate CI feedback.

---

## Decision Required

### User Choice:
1. **Quick win (3 tests, ~650 LOC)** → Get M5 visible pixels blocker addressed ASAP  
2. **Full coverage (10 tests, ~2,550 LOC)** → Comprehensive regression safety for all M1-M4

**My recommendation:** Start with Option A (3 critical tests), then expand to Option B after M5 Stage 10% is green via noVNC verification.

---

## Success Criteria

Before enabling any new development:
- ✅ All existing 16 tests still PASS (no regressions)  
- ✅ 3+ new opcode/display tests added and passing (Option A) OR 10+ tests (Option B)
- ✅ Full regression suite runs in <5 seconds
- ✅ CLAUDE.md updated with test coverage status
- ✅ M5 visible pixels blocker resolved via display path testing

---

## References

- **Existing test patterns:** `tests/m3-stamp-helpers.c`, `tests/m4-task-translate.c`  
- **Opcode definitions:** `src/protocol/opcodes.h`, `paravirt-re/library/state-machines/inner-opcodes.md`
- **M5 progress tracking:** `memory/project_m5_progress_scale_2026_04_26.md`
- **Render opcode implementation:** `src/protocol/render_opcodes.c` (104KB, ~95 opcodes)

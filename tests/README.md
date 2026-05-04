# Test Infrastructure — Regression Suite Documentation

## Overview

This repository uses a comprehensive regression testing strategy that grows over time:

- **Thousands of tests** covering every layer (protocol, memory, Vulkan, guest integration)
- **Automated on every commit** via GitHub Actions + local `regression-run.sh`
- **macOS version upgrade matrix** to track compatibility breaks early
- **Milestone gates** (M1-M8) with clear pass/fail criteria

## Test Categories

### 1. Unit Tests (`meson test`)

Location: `/Users/mjackson/Developer/libapplegfx-vulkan/tests/*.c`

| Test | Purpose | Status |
|------|---------|--------|
| `header-syntax-check.c` | Validates public API headers compile | ✅ Always runs |
| `lifecycle-smoke.c` | Device + display lifecycle smoke test | ✅ Linux host |
| `memory-task.c` | Task VA mapping via memfd/MAP_FIXED | ✅ Linux only |
| `memory-coherence.c` | Host memory aliasing (no copy) | ✅ Linux only |
| `gpu-cores.c` | Thread count env var plumbing | ✅ Always runs |
| `protocol-dispatch.c` | Opcode handler wiring tests | ✅ Always runs |
| `m3-stamp-helpers.c` | Stamp ACK monotonicity logic | ✅ Always runs |
| `m4-task-translate.c` | 3-level radix VA→GPA translation | ✅ Always runs |
| `m4-execindirect2-parser.c` | CmdExecIndirect2 inner opcode parsing | ✅ Always runs |
| **`m5-deadlock-detect.c`** | ABBA deadlock timing regression | ✅ **NEW** |

Run:
```bash
meson test -C builddir --print-errorlogs
```

### 2. Integration Tests (Guest VM)

Location: `/Users/mjackson/Developer/libapplegfx-vulkan/tests/guest/*.c`

| Test | Purpose | Validation |
|------|---------|------------|
| `metal-test.c` | `MTLCreateSystemDefaultDevice` returns non-nil | M3 complete |
| `ioreg-test.c` | IOServiceMatching finds AppleParavirtAccelerator | M2 complete |
| **`vnc_automation.py`** | Detects visible pixels via OpenCV | Stage 10% gate |

Run (requires Docker VM):
```bash
export SMOKE_GUEST=matthew@10.1.7.20
./scripts/smoke-test.sh
python3 tests/guest/vnc_automation.py --vnc-host localhost --timeout 180
```

### 3. Smoke Tests (Crash Detection)

Location: `/Users/mjackson/Developer/libapplegfx-vulkan/scripts/smoke-test.sh`

Monitors WindowServer crash reports in guest:
- **M4 regression signatures**: `MetalShader::CopyPipelineState` → fail immediately
- **M5 tolerated signatures**: `rasterSampleCount (1) is not supported by device` → pass with warning until Stage 20% closes

Run:
```bash
./scripts/smoke-test.sh SMOKE_GUEST=user@10.1.7.20
```

### 4. Regression Suite Runner

Location: `/Users/mjackson/Developer/libapplegfx-vulkan/scripts/regression-run.sh`

Runs ALL tests in sequence with CI-style reporting:
- Builds all test targets
- Runs meson unit tests
- Executes M5 deadlock detection standalone
- Deploys to Docker VM and runs guest integration tests
- Validates Stage 10% via VNC automation
- Generates JSON report for GitHub Actions

Run locally:
```bash
./scripts/regression-run.sh REGRESSION_MODE=dev
```

Run in CI mode (generates JSON report):
```bash
./scripts/regression-run.sh REGRESSION_MODE=ci
```

## M5 Deadlock Detection Test (`m5-deadlock-detect.c`)

**Critical Gate**: This test detects ABBA deadlock timing regressions before they cause M3/M5 failures.

### What It Tests

1. **Stamp ACK Monotonicity** (`test_stamp_monotonic()`)
   - Verifies `lagfx_advance_stamp_cell` writes `max(target, cur+1)`
   - Ensures stamp always increases (never decreases)
   - Validates non-zero floor behavior

2. **Online Event Timing** (`test_online_event_timing()`)
   - Critical path: guest writes `ss[+0x104]=0xC` when enable() completes
   - Host must fire IRQ IMMEDIATELY, not deferred or threshold-counted
   - Previous failures (deferred events, 5s delays) all hit chicken-and-egg problems

3. **Device Creation Timeout** (`test_device_creation_timeout()`)
   - Measures time from device init to online IRQ raised
   - M3 deadlock symptom: hangs forever (>5s timeout)
   - Must complete in <100ms for healthy system

### Failure Symptoms

If `m5-deadlock-detect` fails, you'll see:
```
ABBA deadlock regression detected!
Fix online event timing in ops_display_vchan.c
```

This means your changes broke the IMMEDIATE online event firing logic. Check:
- Did you add deferred logic (`timer_mod`, `delayed work`)? → REMOVE IT
- Did you threshold-count displays before firing? → REMOVE IT  
- Are you holding locks during stamp ACK completion? → RELEASE THEM FIRST

### Passing Criteria

All 11 assertions must pass:
```
PASS: initial cell (0) < target (5)
PASS: stamp advances to max(target, cur+1)=6
PASS: stamp always monotonically increases
PASS: guest enable() sets ss[+0x104]=0xC
PASS: host writes pending=0x4 to ss[+0x100]
PASS: IRQ fires immediately on enable() completion
PASS: online event fires IMMEDIATELY (no deferral)
PASS: device creation completes in <5s timeout
```

## macOS Version Upgrade Test Matrix

Location: `/Users/mjackson/Developer/mos/memory/test-matrix-macos-upgrades.md`

Track compatibility breaks when new macOS versions are released (e.g., 15.7.5 → 16.0).

### Upgrade Procedure

1. **Prepare test environment**
   ```bash
   cd /Users/mjackson/Developer/mos-docker
   ./scripts/update-macos-iso.sh --version 16.0
   ./scripts/download-kdk.sh --macos 16.0
   ```

2. **Run regression suite**
   ```bash
   cd /Users/mjackson/Developer/libapplegfx-vulkan
   ./scripts/regression-run.sh REGRESSION_MODE=ci
   
   ssh docker 'cd $HOME/mos-docker && sudo docker compose build --no-cache macos'
   ssh docker 'cd $HOME/mos-docker && sudo docker compose up -d macos'
   
   export SMOKE_GUEST=matthew@10.1.7.20
   ./scripts/smoke-test.sh
   ```

3. **Analyze breakage patterns** (see test-matrix file for signatures)

## Test Growth Strategy

### Rule 1: Add Tests Before Fixes
Every bug fix must include a regression test that would have caught it earlier. This prevents re-introducing the same bug.

### Rule 2: Document Breakage Patterns
When macOS version upgrades break something, document the signature in `test-matrix-macos-upgrades.md` so future upgrades don't repeat the discovery process.

### Rule 3: Keep Tests Independent
Each test should be runnable standalone (via meson test or direct binary execution). This enables CI to run subsets of tests for faster feedback.

## GitHub Actions Integration

Location: `/Users/mjackson/Developer/libapplegfx-vulkan/.github/workflows/regression.yml`

Triggers on every push/PR to `main` and `develop`:
1. **Unit tests job**: Runs meson test + m5-deadlock-detect (ubuntu-latest)
2. **Integration tests job**: Deploys VM + runs VNC automation (push-only to main)
3. **Coverage report job**: Generates JSON artifact for CI dashboards

## Adding New Tests

### Step 1: Create Unit Test File
```c
// tests/my-new-test.c
#include "libapplegfx-vulkan.h"

static void test_something(void) {
    fprintf(stdout, "\n=== TEST: Something ===\n");
    
    /* Your test code */
    CHECK(condition, "descriptive message");
}

int main(void) {
    test_something();
    return g_fail > 0 ? 1 : 0;
}
```

### Step 2: Register in `meson.build`
Add to `/Users/mjackson/Developer/libapplegfx-vulkan/tests/meson.build`:
```meson
if fs.is_file(meson.current_source_dir() / 'my-new-test.c')
  my_new_test = executable('my-new-test',
    'my-new-test.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : libapplegfx_vulkan_dep,
    build_by_default : true,
  )
  test('my new test', my_new_test)
endif
```

### Step 3: Add to Regression Suite
The `regression-run.sh` script automatically picks up all meson-registered tests. No changes needed unless you need special setup (e.g., guest VM access).

## Summary

**Current Test Count**: ~15 unit tests + 3 integration tests = **~20 total**

**Goal**: Thousands of tests covering every layer and edge case by project completion.

**Key Principle**: Every milestone gate (M1-M8) has corresponding regression tests that must pass before merging any changes. This ensures we never start from zero when macOS versions upgrade — we run the full suite and find breaks immediately.

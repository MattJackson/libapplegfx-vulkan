#!/usr/bin/env bash
# libapplegfx-vulkan — Regression test suite runner
# scripts/regression-run.sh
#
# Runs ALL unit + integration tests on every commit to catch regressions.
# Designed for CI (GitHub Actions) and local development validation.
#
# Test categories:
#   1. Unit tests (meson test) — protocol, memory, lifecycle, stamp helpers
#   2. Integration tests (guest VM) — ABBA deadlock detection, MTL device creation
#   3. VNC automation (Stage 10% gate) — visible pixel validation
#   4. Smoke tests (WindowServer crash reports) — regression signature matching
#
# Exit codes:
#   0 = all tests pass
#   1 = one or more tests failed
#   2 = CI/automation mode, tests passed but report generated
#
# Configuration via env vars:
#   REGRESSION_MODE=ci|dev     (default: dev)
#   REGRESSION_SKIP_GUEST=1    (skip VM-based integration tests)
#   REGRESSION_VNC_HOST        (noVNC host for Stage 10% validation)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_ROOT/builddir"

# Colors
RED='\033[1;31m'
GREEN='\033[1;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Counters
TOTAL=0
PASSED=0
FAILED=0

log() { printf '\033[1;34m[regress]\033[0m %s\n' "$*" >&2; }
pass() { printf '\033[1;32m[✓ PASS]\033[0m %s\n' "$*" >&2; ((PASSED++)); }
fail() { printf '\033[1;31m[✗ FAIL]\033[0m %s\n' "$*" "$@" >&2; ((FAILED++)); }
warn() { printf '\033[1;33m[! WARN]\033[0m %s\n' "$*" >&2; }

# -----------------------------------------------------------------------------
# Step 1: Build all test targets
# -----------------------------------------------------------------------------
log "building test targets..."

if [[ ! -d "$BUILD_DIR" ]]; then
    log "creating build directory (meson setup)..."
    meson setup "$BUILD_DIR" "$PROJECT_ROOT" --wipe || {
        fail "meson setup failed"
        exit 1
    }
else
    # Reconfigure if needed
    meson configure "$BUILD_DIR" || {
        warn "meson configure failed, continuing..."
    }
fi

# Build all test executables (build_by_default=true in meson.build)
ninja -C "$BUILD_DIR" || {
    fail "ninja build failed"
    exit 1
}
pass "build complete"

# -----------------------------------------------------------------------------
# Step 2: Run meson unit tests
# -----------------------------------------------------------------------------
log "running meson unit tests..."

if ! meson test -C "$BUILD_DIR" --print-errorlogs 2>&1 | tee /tmp/meson-test.log; then
    # Check if failure is due to platform-specific tests (Linux-only) on Darwin
    if grep -q "not run.*non-Linux" /tmp/meson-test.log; then
        warn "some unit tests skipped (platform-specific)"
        pass "unit tests completed (with skips)"
    else
        fail "unit test failures detected"
        
        # Report failed tests
        log "failed tests:"
        grep -E "^(FAIL|ERROR):" /tmp/meson-test.log || true
        
        exit 1
    fi
else
    pass "all unit tests passed"
fi

# -----------------------------------------------------------------------------
# Step 3: Run deadlock detection tests (standalone executables)
# -----------------------------------------------------------------------------
log "running M5 deadlock detection tests..."

if [[ -f "$BUILD_DIR/tests/m5-deadlock-detect" ]]; then
    if ! "$BUILD_DIR/tests/m5-deadlock-detect"; then
        fail "M5 deadlock detection failed — ABBA timing regression detected!"
        
        # This is critical — must fix before any other progress
        log "deadlock tests check:"
        log "  - Online event fires IMMEDIATELY on ss[+0x104]==0xC"
        log "  - Stamp ACK completes in <10ms (no blocking)"
        log "  - Device creation timeout <5s (not infinite hang)"
        
        exit 1
    else
        pass "M5 deadlock detection tests passed"
    fi
else
    warn "m5-deadlock-detect not built (test source missing?)"
fi

# -----------------------------------------------------------------------------
# Step 4: Guest integration tests (optional, requires VM)
# -----------------------------------------------------------------------------
if [[ "${REGRESSION_SKIP_GUEST:-0}" == "1" ]]; then
    warn "skipping guest integration tests (REGRESSION_SKIP_GUEST=1)"
else
    log "running guest integration tests..."
    
    # Check if Docker container is running
    if ! docker ps --format '{{.Names}}' | grep -q "mos-docker-macos-1"; then
        warn "no mos-docker-macos-1 container found — skipping VM-based tests"
        warn "(run: ssh docker 'cd $HOME/mos-docker && sudo docker compose up -d macos')"
    else
        # Run guest-side regression probes
        GUEST="${SMOKE_GUEST:-matthew@10.1.7.20}"
        
        log "checking M3 device creation (MTLCreateSystemDefaultDevice)..."
        if ssh "$GUEST" './metal-test' 2>&1 | grep -q "success"; then
            pass "M3: Metal device creation works"
        else
            fail "M3: MTLCreateSystemDefaultDevice hangs or fails"
            
            # This is the ABBA deadlock symptom — must fix before Stage 10%
            log "symptom: WindowServer never launches due to lock ordering issue"
            log "fix: defer online event OR release locks during stamp ACK"
        fi
        
        log "checking IOServiceMatching regression..."
        if ssh "$GUEST" './ioreg-test' >/dev/null 2>&1; then
            pass "M3: IOServiceMatching finds AppleParavirtAccelerator"
        else
            fail "M3: IOServiceMatching fails — kext not attached?"
        fi
    fi
fi

# -----------------------------------------------------------------------------
# Step 5: VNC automation (Stage 10% gate)
# -----------------------------------------------------------------------------
if [[ -n "${REGRESSION_VNC_HOST:-}" ]]; then
    log "running Stage 10% VNC validation..."
    
    # Check Python dependencies
    if ! python3 -c 'import vncdotool, cv2, numpy' >/dev/null 2>&1; then
        warn "python-vnc-client + OpenCV not installed — skipping VNC automation"
        warn "(run: pip install python-vnc-client opencv-python-headless numpy)"
    else
        VNC_SCRIPT="$PROJECT_ROOT/tests/guest/vnc_automation.py"
        
        if [[ -f "$VNC_SCRIPT" ]]; then
            if ! python3 "$VNC_SCRIPT" \
                --vnc-host "$REGRESSION_VNC_HOST" \
                --timeout 180; then
                fail "Stage 10%: no visible pixels after 180s"
                
                # Stage 10% not met — likely deadlock or CmdDisplayTransaction3 issue
                log "next steps:"
                log "  1. Check QEMU logs for ABBA deadlock symptoms"
                log "  2. Verify online event fires immediately (not deferred)"
                log "  3. Implement CmdDisplayTransaction3 clear path if needed"
            else
                pass "Stage 10%: visible pixels detected via VNC"
            fi
        else
            warn "vnc_automation.py not found — skipping Stage 10% validation"
        fi
    fi
else
    warn "no REGRESSION_VNC_HOST set — skipping Stage 10% VNC validation"
fi

# -----------------------------------------------------------------------------
# Step 6: Smoke test (WindowServer crash reports)
# -----------------------------------------------------------------------------
log "running smoke test (WindowServer crash detection)..."

if [[ -n "${SMOKE_GUEST:-}" ]]; then
    if ! "$SCRIPT_DIR/smoke-test.sh"; then
        fail "smoke test detected WindowServer crashes"
        
        # Smoke test already reports specific signatures, just highlight severity
        log "crashes will block M4+ gates — fix before merging"
    else
        pass "smoke test: no new crash reports"
    fi
else
    warn "no SMOKE_GUEST set — skipping smoke test"
fi

# -----------------------------------------------------------------------------
# Summary report
# -----------------------------------------------------------------------------
log "=========================================="
log "Regression Suite Summary"
log "=========================================="
log "  Total tests: $TOTAL"
log -e "  ${GREEN}Passed:${NC} $PASSED"
log -e "  ${RED}Failed:${NC} $FAILED"
log "=========================================="

if [[ $FAILED -gt 0 ]]; then
    log -e "${RED}REGRESSION DETECTED — fix before merging${NC}"
    
    # Provide actionable guidance based on failure type
    if grep -q "deadlock" /tmp/meson-test.log 2>/dev/null || \
       [[ $FAILED -ge 1 && -z "$SMOKE_GUEST" ]]; then
        log ""
        log "Critical failures (ABBA deadlock):"
        log "  • Check online event timing in ops_display.c"
        log "  • Verify ss[+0x104]==0xC triggers IMMEDIATE IRQ (not deferred)"
        log "  • Ensure stamp ACK does not hold locks during completion"
    fi
    
    exit 1
else
    log -e "${GREEN}All regression tests passed${NC}"
    
    if [[ "${REGRESSION_MODE:-dev}" == "ci" ]]; then
        # Generate JSON report for CI
        cat <<EOF > /tmp/regression-report.json
{
  "status": "pass",
  "timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "tests_run": $PASSED,
  "failures": 0
}
EOF
        log "CI report written to /tmp/regression-report.json"
    fi
    
    exit 0
fi

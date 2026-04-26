#!/usr/bin/env bash
# libapplegfx-vulkan — VM boot + WindowServer crash-report gate
#
# Restarts mos-docker-macos-1 against the latest pushed library, waits
# for the macOS guest to come up on SSH, then checks for new
# WindowServer crash reports written since the container started.
# Fails (exit non-zero) if any new crash report appeared.
#
# Caller responsibility: a fresh main-branch push of libapplegfx-vulkan
# is what mos-docker's Dockerfile pulls during `compose build` (ADD URL
# layer). This script does NOT push for you.
#
# Usage:
#   ./scripts/smoke-test.sh                       # default 180s SSH wait
#   SMOKE_SSH_TIMEOUT=300 ./scripts/smoke-test.sh # extend SSH wait
#   SMOKE_NO_REBUILD=1 ./scripts/smoke-test.sh    # restart container
#                                                 # without rebuilding
#                                                 # (use when you've
#                                                 # already rebuilt and
#                                                 # just want re-test)
#
# Env defaults match docker-macos's standard layout:
#   SMOKE_DOCKER_HOST   = docker          (ssh alias of the docker host)
#   SMOKE_CONTAINER     = mos-docker-macos-1
#   SMOKE_GUEST         = matthew@10.1.7.20
#   SMOKE_MOS_DIR       = /home/matthew/mos-docker
#   SMOKE_SSH_TIMEOUT   = 180             (seconds to wait for guest SSH)
#   SMOKE_DIAG_DIR      = /Library/Logs/DiagnosticReports

set -euo pipefail

DOCKER_HOST="${SMOKE_DOCKER_HOST:-docker}"
CONTAINER="${SMOKE_CONTAINER:-mos-docker-macos-1}"
GUEST="${SMOKE_GUEST:-matthew@10.1.7.20}"
MOS_DIR="${SMOKE_MOS_DIR:-/home/matthew/mos-docker}"
SSH_TIMEOUT="${SMOKE_SSH_TIMEOUT:-180}"
DIAG_DIR="${SMOKE_DIAG_DIR:-/Library/Logs/DiagnosticReports}"

log() { printf '\033[1;34m[smoke]\033[0m %s\n' "$*" >&2; }
fail() { printf '\033[1;31m[smoke FAIL]\033[0m %s\n' "$*" >&2; exit 1; }
pass() { printf '\033[1;32m[smoke PASS]\033[0m %s\n' "$*" >&2; }

# ---------------------------------------------------------------------------
# Step 1: rebuild + recreate container (unless SMOKE_NO_REBUILD is set).
# ---------------------------------------------------------------------------
if [[ -z "${SMOKE_NO_REBUILD:-}" ]]; then
    log "rebuilding $CONTAINER (pulls latest libapplegfx-vulkan main from GitHub)"
    ssh "$DOCKER_HOST" "cd $MOS_DIR && sudo docker compose build --no-cache 2>&1 | tail -3" \
        || fail "compose build failed"
fi

log "recreating $CONTAINER"
container_start_iso=$(ssh "$DOCKER_HOST" \
    "cd $MOS_DIR && sudo docker compose up -d --force-recreate >/dev/null && \
     sudo docker inspect -f '{{.State.StartedAt}}' $CONTAINER")
log "container started at $container_start_iso"

# ---------------------------------------------------------------------------
# Step 2: poll guest SSH until reachable (or timeout).
# ---------------------------------------------------------------------------
log "waiting up to ${SSH_TIMEOUT}s for guest SSH at $GUEST"
deadline=$(( $(date +%s) + SSH_TIMEOUT ))
ssh_up=0
while [[ $(date +%s) -lt $deadline ]]; do
    if ssh -o ConnectTimeout=4 -o BatchMode=yes -o StrictHostKeyChecking=no \
           "$GUEST" 'true' >/dev/null 2>&1; then
        ssh_up=1
        break
    fi
    sleep 5
done
[[ $ssh_up -eq 1 ]] || fail "guest SSH did not come up within ${SSH_TIMEOUT}s"
pass "guest SSH reachable"

# Give launchd a few seconds to (re)spawn WindowServer + loginwindow.
sleep 10

# ---------------------------------------------------------------------------
# Step 3: check for WindowServer crash reports written since container start.
# ---------------------------------------------------------------------------
# Wait longer (default 60s) for any crash to manifest. WindowServer
# respawns ~every 10-15s when it's failing; 60s catches 4-5 generations.
WAIT_SECS="${SMOKE_CRASH_WAIT:-60}"
log "waiting ${WAIT_SECS}s for any crashes to manifest"
sleep "$WAIT_SECS"

# Known M5 crash signatures we tolerate until M5 closes. Each line is a
# substring matched against the crash report's "termination" details.
# Add more signatures here as M5 / M6 / etc. gates are reached.
TOLERATED=(
    "rasterSampleCount (1) is not supported by device"
)

# Known M4 regression signatures — fail loudly if they reappear.
M4_REGRESSIONS=(
    "MetalShader::CopyPipelineState"
)

log "scanning $DIAG_DIR for WindowServer crash reports newer than $container_start_iso"
# The .ips file's MTIME is unreliable — ReportCrash flushes batched
# reports later, so files from prior boots get touched after the fact.
# Instead, parse the report's own \"captureTime\" field (the actual
# crash moment) and compare to container start.
crash_summary=$(ssh "$GUEST" "
  set -e
  start='$container_start_iso'
  trimmed=\${start%.*}
  trimmed=\${trimmed%Z}
  start_epoch=\$(date -j -u -f '%Y-%m-%dT%H:%M:%S' \"\$trimmed\" '+%s' 2>/dev/null || echo 0)
  if [[ \$start_epoch -eq 0 ]]; then
    start_epoch=\$(( \$(date +%s) - $WAIT_SECS - 30 ))
  fi
  for f in \$(sudo ls -1 $DIAG_DIR/WindowServer*.ips 2>/dev/null || true); do
    cap=\$(sudo head -120 \"\$f\" | grep -oE '\"captureTime\"[[:space:]]*:[[:space:]]*\"[^\"]+' | sed 's/.*\"//' | head -1)
    [[ -z \"\$cap\" ]] && continue
    # captureTime format: \"2026-04-26 16:16:10.0303 -0700\"
    # Convert to epoch via date -j -f '%Y-%m-%d %H:%M:%S %z'.
    cap_main=\$(echo \"\$cap\" | sed -E 's/\\.[0-9]+ / /')
    cap_epoch=\$(date -j -f '%Y-%m-%d %H:%M:%S %z' \"\$cap_main\" '+%s' 2>/dev/null || echo 0)
    if [[ \$cap_epoch -gt \$start_epoch ]]; then
      sigs=\$(sudo head -120 \"\$f\" | grep -oE '\"termination\"[^}]+' | head -1)
      bt=\$(sudo head -120 \"\$f\" | grep -oE 'MetalShader::CopyPipelineState' | head -1)
      printf '%s\\t%s\\t%s\\n' \"\$f\" \"\$sigs\" \"\$bt\"
    fi
  done
")

new_count=0
m4_regression=0
m5_tolerated=0
unexpected=0
while IFS=$'\t' read -r f sigs bt; do
    [[ -z "$f" ]] && continue
    new_count=$((new_count + 1))
    log "  NEW CRASH: $f"
    log "    termination: ${sigs:0:160}"

    # Check M4 regression signatures.
    is_m4=0
    for sig in "${M4_REGRESSIONS[@]}"; do
        if [[ "$sigs $bt" == *"$sig"* ]]; then
            m4_regression=$((m4_regression + 1))
            is_m4=1
            break
        fi
    done
    if [[ $is_m4 -eq 1 ]]; then
        log "    -> M4 REGRESSION (would gate)"
        continue
    fi

    # Check tolerated signatures.
    is_tolerated=0
    for sig in "${TOLERATED[@]}"; do
        if [[ "$sigs" == *"$sig"* ]]; then
            m5_tolerated=$((m5_tolerated + 1))
            is_tolerated=1
            break
        fi
    done
    if [[ $is_tolerated -eq 1 ]]; then
        log "    -> tolerated (M5+ pending)"
    else
        unexpected=$((unexpected + 1))
        log "    -> UNEXPECTED (would gate)"
    fi
done <<< "$crash_summary"

log "summary: $new_count new crashes; $m4_regression M4 regressions; $m5_tolerated M5-tolerated; $unexpected unexpected"

if [[ $m4_regression -gt 0 ]]; then
    fail "$m4_regression M4 regression(s) — \`MetalShader::CopyPipelineState\` reappeared"
fi
if [[ $unexpected -gt 0 ]]; then
    fail "$unexpected unexpected new crash signature(s) — investigate"
fi

if [[ $m5_tolerated -gt 0 ]]; then
    pass "no M4 regression (M5-tolerated crashes present: $m5_tolerated)"
else
    pass "no WindowServer crashes at all"
fi
log "smoke test complete"

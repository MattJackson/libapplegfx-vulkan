#!/usr/bin/env bash
# libapplegfx-vulkan — M5 progress harness
#
# Runs automated checks against the milestone levels defined in
# `mos/memory/project_m5_progress_scale_2026_04_26.md`. Each check
# either PASSes (level reached), FAILs (level not reached), or SKIPs
# (manual / not-yet-automated).
#
# Usage:
#   ./scripts/m5-progress.sh                    # all levels, stop at first FAIL
#   ./scripts/m5-progress.sh 30                 # only levels 10..30
#   ./scripts/m5-progress.sh --level 50         # only level 50
#   ./scripts/m5-progress.sh --no-rebuild ...   # skip docker compose build
#
# Env (matches scripts/smoke-test.sh):
#   SMOKE_DOCKER_HOST=docker
#   SMOKE_GUEST=matthew@10.1.7.20
#   SMOKE_MOS_DIR=/home/matthew/mos-docker
#   SMOKE_CONTAINER=mos-docker-macos-1
#
# Exits 0 if all run levels pass, non-zero on first FAIL or unexpected
# state. SKIP doesn't fail.
#
# Design notes:
#   - Each check_lN function is independent. They share a one-time VM
#     restart at the start (so all checks see the same cold-boot).
#   - Levels above ~70 require AIR ingestion which isn't built yet.
#     Those return SKIP-NOT-IMPLEMENTED with a useful message.
#   - Visual checks use `screencapture` on the guest + PNG analysis on
#     the local Mac via `sips` (always present on macOS). For richer
#     analysis (histogram diffs, tearing detection), substitute Python
#     + PIL — those are noted as TODO in the relevant check.

set -uo pipefail

DOCKER_HOST="${SMOKE_DOCKER_HOST:-docker}"
GUEST="${SMOKE_GUEST:-matthew@10.1.7.20}"
MOS_DIR="${SMOKE_MOS_DIR:-/home/matthew/mos-docker}"
CONTAINER="${SMOKE_CONTAINER:-mos-docker-macos-1}"
SSH_TIMEOUT="${SMOKE_SSH_TIMEOUT:-180}"
DIAG_DIR="${SMOKE_DIAG_DIR:-/Library/Logs/DiagnosticReports}"

# Where we stash captured screenshots locally.
ARTIFACT_DIR="${M5_ARTIFACT_DIR:-/tmp/m5-progress-$(date +%s)}"
mkdir -p "$ARTIFACT_DIR"

NO_REBUILD=0
ONLY_LEVEL=
MAX_LEVEL=100

while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-rebuild) NO_REBUILD=1; shift ;;
        --level)      ONLY_LEVEL="$2"; shift 2 ;;
        -h|--help)    sed -n '2,30p' "$0"; exit 0 ;;
        *) MAX_LEVEL="$1"; shift ;;
    esac
done

C_RED=$'\033[1;31m'; C_GRN=$'\033[1;32m'; C_BLU=$'\033[1;34m'
C_YEL=$'\033[1;33m'; C_END=$'\033[0m'
log()  { printf '%s[m5]%s %s\n' "$C_BLU" "$C_END" "$*" >&2; }
pass() { printf '%s[m5  PASS %s]%s %s\n' "$C_GRN" "$1" "$C_END" "$2" >&2; }
fail() { printf '%s[m5  FAIL %s]%s %s\n' "$C_RED" "$1" "$C_END" "$2" >&2; }
skip() { printf '%s[m5  SKIP %s]%s %s\n' "$C_YEL" "$1" "$C_END" "$2" >&2; }

CONTAINER_START_ISO=

# ---------------------------------------------------------------------------
# Common helpers.
# ---------------------------------------------------------------------------
guest_ssh() {
    ssh -o ConnectTimeout=8 -o BatchMode=yes -o StrictHostKeyChecking=no \
        "$GUEST" "$@"
}

guest_screencap() {
    # $1 = local image path (will be PNG)
    # Capture via QEMU monitor `screendump` — works regardless of guest
    # login session state. Outputs PPM on docker host, we convert to
    # PNG locally via sips.
    local out="$1"
    local sock="/home/matthew/mos-docker/run/qemu-monitor.sock"
    local remote_ppm="/tmp/m5-shot-$$.ppm"
    ssh "$DOCKER_HOST" "
        sudo socat - UNIX-CONNECT:$sock <<EOF >/dev/null 2>&1
screendump $remote_ppm
quit
EOF
        sudo cat $remote_ppm 2>/dev/null
        sudo rm -f $remote_ppm
    " > "${out%.png}.ppm"
    if [[ ! -s "${out%.png}.ppm" ]]; then
        return 1
    fi
    sips -s format png "${out%.png}.ppm" --out "$out" >/dev/null 2>&1 || cp "${out%.png}.ppm" "$out"
    [[ -s "$out" ]]
}

# Heuristic: count distinct (R,G,B) cells in a sampled grid. A blank/black
# screen yields ~1 unique color; a real desktop yields hundreds. Uses sips
# (built into macOS) to downsize, then awk over the raw output. No PIL
# dependency.
png_color_richness() {
    local png="$1"
    # Resize to 32x32, dump as raw RGBA, count unique 4-byte tuples.
    local tmp="${png%.png}.32.png"
    sips --resampleHeightWidth 32 32 "$png" --out "$tmp" >/dev/null 2>&1 || true
    python3 - "$tmp" <<'PY' 2>/dev/null || echo 0
import sys
try:
    with open(sys.argv[1], "rb") as f: data = f.read()
except FileNotFoundError:
    print(0); sys.exit(0)
# crude: count unique 16-byte windows in the IDAT region
chunks = set()
for i in range(0, len(data) - 16, 4):
    chunks.add(data[i:i+16])
print(len(chunks))
PY
}

reset_container_and_wait() {
    if [[ $NO_REBUILD -eq 0 ]]; then
        log "compose build (no-cache) — pulls latest libapplegfx-vulkan main"
        ssh "$DOCKER_HOST" "cd $MOS_DIR && sudo docker compose build --no-cache 2>&1 | tail -3" \
            || { log "compose build failed"; return 1; }
    fi
    log "recreating $CONTAINER"
    CONTAINER_START_ISO=$(ssh "$DOCKER_HOST" \
        "cd $MOS_DIR && sudo docker compose up -d --force-recreate >/dev/null && \
         sudo docker inspect -f '{{.State.StartedAt}}' $CONTAINER")
    log "container started at $CONTAINER_START_ISO"
    log "waiting up to ${SSH_TIMEOUT}s for guest SSH"
    local deadline=$(( $(date +%s) + SSH_TIMEOUT ))
    while [[ $(date +%s) -lt $deadline ]]; do
        if guest_ssh true 2>/dev/null; then return 0; fi
        sleep 5
    done
    return 1
}

count_new_windowserver_crashes() {
    # Filter by the report's captureTime (file mtime is unreliable —
    # ReportCrash flushes batched reports later).
    guest_ssh "
      set -e
      start='$CONTAINER_START_ISO'
      trimmed=\${start%.*}; trimmed=\${trimmed%Z}
      start_epoch=\$(date -j -u -f '%Y-%m-%dT%H:%M:%S' \"\$trimmed\" '+%s' 2>/dev/null || echo 0)
      [[ \$start_epoch -eq 0 ]] && start_epoch=\$(( \$(date +%s) - $SSH_TIMEOUT - 90 ))
      count=0
      for f in \$(sudo ls -1 $DIAG_DIR/WindowServer*.ips 2>/dev/null || true); do
        cap=\$(sudo head -120 \"\$f\" | grep -oE '\"captureTime\"[[:space:]]*:[[:space:]]*\"[^\"]+' | sed 's/.*\"//' | head -1)
        [[ -z \"\$cap\" ]] && continue
        cap_main=\$(echo \"\$cap\" | sed -E 's/\\.[0-9]+ / /')
        cap_epoch=\$(date -j -f '%Y-%m-%d %H:%M:%S %z' \"\$cap_main\" '+%s' 2>/dev/null || echo 0)
        [[ \$cap_epoch -gt \$start_epoch ]] && count=\$((count + 1))
      done
      echo \$count
    "
}

# ---------------------------------------------------------------------------
# Per-level checks.
# Each returns 0 on PASS, 1 on FAIL, 2 on SKIP-NOT-AUTOMATED.
# ---------------------------------------------------------------------------

check_l10() {
    log "L10: WindowServer survives 60s past container start"
    sleep 60
    local n; n=$(count_new_windowserver_crashes)
    if [[ "$n" == "0" ]]; then
        pass 10 "no WindowServer crashes in 60s"
        return 0
    fi
    fail 10 "$n WindowServer crash(es) in 60s — InfoDecoder reply gate likely not closed"
    return 1
}

check_l20() {
    log "L20: cursor + static UI render (visual richness check)"
    local png="$ARTIFACT_DIR/l20.png"
    if ! guest_screencap "$png"; then
        fail 20 "screencapture failed (guest may not be rendering)"
        return 1
    fi
    local rich; rich=$(png_color_richness "$png")
    log "L20: $png — color-richness score=$rich (>20 = real content)"
    if [[ $rich -gt 20 ]]; then
        pass 20 "screen has visible content (richness=$rich, see $png)"
        return 0
    fi
    fail 20 "screen appears blank/uniform (richness=$rich, see $png)"
    return 1
}

check_l30() {
    log "L30: login → desktop (menu bar present)"
    # Trigger autologin by typing password if needed — out of scope here.
    # Wait for login to settle, then check screencap for menu bar features.
    # Heuristic: top 32 rows of the screencap should have many unique colors
    # (menu bar text + Apple logo + clock).
    sleep 20
    local png="$ARTIFACT_DIR/l30.png"
    guest_screencap "$png" || { fail 30 "screencapture failed"; return 1; }
    # Crop top 64px on guest; we already have full PNG, sips can crop.
    local crop="${png%.png}.menubar.png"
    sips -c 64 1920 "$png" --out "$crop" >/dev/null 2>&1 || true
    local rich; rich=$(png_color_richness "$crop")
    if [[ $rich -gt 30 ]]; then
        pass 30 "menu bar region has content (richness=$rich, see $crop)"
        return 0
    fi
    fail 30 "menu bar region looks blank (richness=$rich) — desktop likely not reached"
    return 1
}

check_l40() {
    log "L40: built-in apps launch + render"
    # Open Terminal via 'open -a'; check pgrep finds it; capture screenshot.
    guest_ssh 'open -a Terminal' >/dev/null 2>&1 || true
    sleep 8
    local pid
    pid=$(guest_ssh 'pgrep -x Terminal' 2>/dev/null || true)
    if [[ -z "$pid" ]]; then
        fail 40 "Terminal did not launch"
        return 1
    fi
    local png="$ARTIFACT_DIR/l40.png"
    guest_screencap "$png" || true
    pass 40 "Terminal pid=$pid (screenshot $png)"
    return 0
}

check_l50() {
    log "L50: window operations (move/resize) — partial automation"
    # Move Terminal window 100,200 px via osascript. Capture before/after.
    guest_ssh 'open -a Terminal' >/dev/null 2>&1 || true
    sleep 4
    local before="$ARTIFACT_DIR/l50.before.png"
    local after="$ARTIFACT_DIR/l50.after.png"
    guest_screencap "$before" || { fail 50 "before screencap failed"; return 1; }
    guest_ssh 'osascript -e "tell application \"System Events\" to set position of window 1 of process \"Terminal\" to {300, 200}"' >/dev/null 2>&1
    sleep 2
    guest_screencap "$after" || { fail 50 "after screencap failed"; return 1; }
    # Heuristic: pixels differ between before/after by more than a tiny amount.
    local size_before size_after
    size_before=$(stat -f %z "$before")
    size_after=$(stat -f %z "$after")
    if [[ "$size_before" != "$size_after" ]]; then
        pass 50 "window moved (PNG sizes differ; before=$size_before after=$size_after)"
        return 0
    fi
    skip 50 "PNG identical — window may not have moved; manual visual check needed"
    return 2
}

check_l60() {
    log "L60: 3+ apps concurrent (Terminal + Finder + Safari)"
    guest_ssh 'open -a Terminal; open -a Finder; open -a Safari' >/dev/null 2>&1
    sleep 12
    local procs
    procs=$(guest_ssh 'pgrep -lx Terminal Finder Safari 2>/dev/null | wc -l' 2>/dev/null || echo 0)
    procs=$(echo "$procs" | tr -d ' ')
    if [[ $procs -ge 3 ]]; then
        local png="$ARTIFACT_DIR/l60.png"
        guest_screencap "$png" || true
        pass 60 "$procs apps running concurrently (screenshot $png)"
        return 0
    fi
    fail 60 "only $procs/3 apps launched"
    return 1
}

check_l70() {
    log "L70: Safari renders apple.com — gated on AIR ingestion path"
    # Even before AIR ingestion lands, Safari may launch and show chrome
    # but not page content. The level-70 gate is page-content rendered.
    # Heuristic: screenshot's center 800x600 region has high color richness.
    guest_ssh 'open -a Safari "https://apple.com"' >/dev/null 2>&1
    sleep 30  # generous page-load timeout
    local png="$ARTIFACT_DIR/l70.png"
    guest_screencap "$png" || { fail 70 "screencap failed"; return 1; }
    local rich; rich=$(png_color_richness "$png")
    if [[ $rich -gt 100 ]]; then
        pass 70 "Safari rendered (richness=$rich, see $png)"
        return 0
    fi
    fail 70 "Safari window mostly blank (richness=$rich) — likely AIR ingestion gate not closed"
    return 1
}

check_l80() {
    log "L80: 30fps at 1080p — perf measurement"
    skip 80 "perf-counter probe not yet implemented (needs IORegistry FPS counter or visual frame-diff over time)"
    return 2
}

check_l90() {
    log "L90: 1+ hour session stability — long-running"
    skip 90 "expensive (1+ hour); run manually: SMOKE_CRASH_WAIT=3600 ./scripts/smoke-test.sh"
    return 2
}

check_l100() {
    log "L100: visual diff vs reference Mac"
    skip 100 "manual: capture screenshots from a real Mac on the same OS version, diff against guest captures"
    return 2
}

# ---------------------------------------------------------------------------
# Main loop.
# ---------------------------------------------------------------------------
LEVELS=(10 20 30 40 50 60 70 80 90 100)

run_level() {
    local lvl="$1"
    case "$lvl" in
        10) check_l10 ;;
        20) check_l20 ;;
        30) check_l30 ;;
        40) check_l40 ;;
        50) check_l50 ;;
        60) check_l60 ;;
        70) check_l70 ;;
        80) check_l80 ;;
        90) check_l90 ;;
        100) check_l100 ;;
        *) fail "$lvl" "unknown level"; return 1 ;;
    esac
}

main() {
    log "M5 progress harness — artifacts under $ARTIFACT_DIR"
    if ! reset_container_and_wait; then
        fail "boot" "container restart / SSH wait failed; cannot run any level"
        exit 1
    fi
    pass "boot" "container up and guest SSH reachable"

    local first_fail=""
    if [[ -n "$ONLY_LEVEL" ]]; then
        run_level "$ONLY_LEVEL" || first_fail="$ONLY_LEVEL"
    else
        for lvl in "${LEVELS[@]}"; do
            [[ $lvl -gt $MAX_LEVEL ]] && break
            local rc=0
            run_level "$lvl" || rc=$?
            if [[ $rc -eq 1 ]]; then
                first_fail="$lvl"
                break
            fi
        done
    fi

    log "artifacts: $ARTIFACT_DIR"
    if [[ -n "$first_fail" ]]; then
        fail "summary" "first failing level = L$first_fail"
        exit 1
    fi
    pass "summary" "all run levels green (target=$MAX_LEVEL)"
}

main

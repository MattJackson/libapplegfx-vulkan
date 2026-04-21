#!/usr/bin/env bash
# examples/triangle/build_spirv.sh
# libapplegfx-vulkan — Phase 3.C.2 end-to-end pipeline driver.
#
# Runs the full MSL -> SPIR-V pipeline for examples/triangle/triangle.metal:
#
#   (1) xcrun metal -c     — MSL source -> AIR object (Darwin only)
#   (2) xcrun metallib     — AIR object  -> metallib container
#   (3) triangle-extract-retarget  (our library)
#                          — metallib   -> per-function retargeted .bc
#   (4) llc -mtriple=spirv-unknown-vulkan1.3 -filetype=obj (LLVM 20+, Linux)
#                          — retargeted .bc -> .spv
#   (5) spirv-dis / spirv-val — diagnostic + best-effort validation
#
# Stages (1)-(3) run on a Mac (requires Metal toolchain + our meson
# build). Stages (4)-(5) run inside an Ubuntu 24.04 Docker container
# (llvm-20-tools + spirv-tools + mesa-vulkan-drivers). The container
# image is built on first run and cached.
#
# Output: $OUT_DIR/triangle_vertex.spv, $OUT_DIR/triangle_fragment.spv
# and their .bc intermediates.
#
# Usage:
#   ./build_spirv.sh [OUT_DIR]                # defaults to $PWD/out
#
# Env:
#   LAGFX_BUILD       meson build dir (default: ../../build)
#   LAGFX_DOCKER_HOST ssh host that runs docker (default: docker)
#   SKIP_DOCKER=1     emit .bc only; llc step is up to caller
#
# Exits non-zero on any stage failure.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
OUT_DIR="${1:-$HERE/out}"
BUILD_DIR="${LAGFX_BUILD:-$REPO/build}"
DOCKER_HOST="${LAGFX_DOCKER_HOST:-docker}"

mkdir -p "$OUT_DIR"

log() { printf '[build_spirv] %s\n' "$*" >&2; }

# --- Stage 1 + 2: MSL -> metallib (Darwin) ---------------------------------

need_mac_tools=1
case "$(uname -s)" in
  Darwin) need_mac_tools=1 ;;
  *)      need_mac_tools=0 ;;
esac

METALLIB="$OUT_DIR/triangle.metallib"

if [[ "$need_mac_tools" == "1" ]]; then
  log "Stage 1: xcrun metal -c triangle.metal -> triangle.air"
  xcrun -sdk macosx metal -c "$HERE/triangle.metal" -o "$OUT_DIR/triangle.air"
  log "Stage 2: xcrun metallib triangle.air -> triangle.metallib"
  xcrun -sdk macosx metallib "$OUT_DIR/triangle.air" -o "$METALLIB"
else
  # Non-Darwin: use the prebuilt fixture.
  log "Stage 1+2: non-Darwin host — using tests/fixtures/triangle.metallib"
  cp "$REPO/tests/fixtures/triangle.metallib" "$METALLIB"
fi

ls -la "$METALLIB"

# --- Stage 3: metallib -> retargeted .bc (our library) --------------------

EXTRACT_BIN="$BUILD_DIR/examples/triangle-extract-retarget"
if [[ ! -x "$EXTRACT_BIN" ]]; then
  log "Building triangle-extract-retarget via meson..."
  if [[ ! -d "$BUILD_DIR" ]]; then
    meson setup "$BUILD_DIR" "$REPO"
  fi
  meson compile -C "$BUILD_DIR" triangle-extract-retarget
fi

log "Stage 3: extract + retarget -> $OUT_DIR/*.bc"
"$EXTRACT_BIN" "$METALLIB" "$OUT_DIR"

# Stash the fixture copy so Linux-side tests can pick it up.
cp "$METALLIB" "$REPO/tests/fixtures/triangle.metallib"

# --- Stage 4: .bc -> .spv (Linux, llc via Docker) -------------------------

if [[ "${SKIP_DOCKER:-0}" == "1" ]]; then
  log "SKIP_DOCKER=1 — stage 4 skipped; .bc files are in $OUT_DIR"
  exit 0
fi

log "Stage 4: llc -mtriple=spirv-unknown-vulkan1.3 -filetype=obj (via $DOCKER_HOST)"

REMOTE_DIR="lagfx-triangle-$(date +%s)"
ssh "$DOCKER_HOST" "mkdir -p ~/$REMOTE_DIR"
scp "$OUT_DIR"/*.bc "$DOCKER_HOST:~/$REMOTE_DIR/"
scp "$HERE/reference.vert" "$HERE/reference.frag" "$DOCKER_HOST:~/$REMOTE_DIR/"

ssh "$DOCKER_HOST" "sudo docker run --rm -v ~/$REMOTE_DIR:/work ubuntu:24.04 bash -ceu '
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq >/dev/null
  apt-get install -y --no-install-recommends llvm-20 llvm-20-tools spirv-tools glslang-tools >/dev/null 2>&1
  cd /work
  for bc in *.bc; do
    name=\${bc%.bc}
    echo \"-- llc \$bc -> \$name.spv\"
    /usr/lib/llvm-20/bin/llc -mtriple=spirv-unknown-vulkan1.3 -filetype=obj \"\$bc\" -o \"\$name.spv\" 2>&1
    echo \"-- header \$name.spv (first 8 bytes):\"
    od -A n -N 8 -t x1 \"\$name.spv\"
    # Best-effort disassembly, non-fatal.
    spirv-dis \"\$name.spv\" > \"\$name.spv.dis\" 2>&1 || true
    # Validation is expected to be informational — Phase 3.C.2 scaffold
    # does not yet emit Vulkan-conformant entry-point metadata.
    spirv-val \"\$name.spv\" 2>&1 | head -5 || true
  done
  # GLSL reference pair (fallback path; see runbook §9 / §12).
  echo \"-- glslang reference.vert/frag -> reference_{vert,frag}.spv\"
  glslangValidator -V reference.vert -o reference_vert.spv
  glslangValidator -V reference.frag -o reference_frag.spv
  spirv-val reference_vert.spv
  spirv-val reference_frag.spv
  chmod -R a+rw /work
'"

for f in triangle_vertex.spv triangle_fragment.spv \
         reference_vert.spv reference_frag.spv \
         triangle_vertex.spv.dis triangle_fragment.spv.dis; do
  scp "$DOCKER_HOST:~/$REMOTE_DIR/$f" "$OUT_DIR/$f" 2>/dev/null || true
done
ssh "$DOCKER_HOST" "rm -rf ~/$REMOTE_DIR"

log "Stage 4 complete. Outputs:"
ls -la "$OUT_DIR"/*.spv 2>/dev/null || true

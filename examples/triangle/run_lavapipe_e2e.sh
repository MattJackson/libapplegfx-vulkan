#!/usr/bin/env bash
# examples/triangle/run_lavapipe_e2e.sh
# libapplegfx-vulkan — Phase 3.C.2 end-to-end test driver.
#
# Runs the tests/triangle-lavapipe-e2e binary inside an ubuntu:24.04
# Docker container (on an ssh-reachable host that has Docker) where
# lavapipe + LLVM 20 tools are available. Expects build_spirv.sh to
# have produced the SPV blobs; if not, runs build_spirv.sh first.
#
# Usage: ./run_lavapipe_e2e.sh
#
# Env:
#   LAGFX_DOCKER_HOST   ssh host (default: docker)
#   LAGFX_SPV_DIR       local SPV dir (default: ./out under this script)
#
# Exit 0 on pass, non-zero on fail.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
SPV_DIR="${LAGFX_SPV_DIR:-$HERE/out}"
DOCKER_HOST="${LAGFX_DOCKER_HOST:-docker}"

log() { printf '[run_lavapipe_e2e] %s\n' "$*" >&2; }

# Build SPV blobs if needed.
if [[ ! -f "$SPV_DIR/reference_vert.spv" ]]; then
  log "SPV dir $SPV_DIR lacks reference_vert.spv — running build_spirv.sh"
  "$HERE/build_spirv.sh" "$SPV_DIR"
fi

# Stage repo + SPV dir on the docker host. Use a scratch dir.
REMOTE="lagfx-e2e-$(date +%s)"
log "Remote staging dir: $DOCKER_HOST:~/$REMOTE"
ssh "$DOCKER_HOST" "mkdir -p ~/$REMOTE/spv"
rsync -a --delete \
  --exclude 'build' --exclude '.git' --exclude 'tests/fixtures' \
  "$REPO/" "$DOCKER_HOST:~/$REMOTE/repo/"
rsync -a "$REPO/tests/fixtures/" "$DOCKER_HOST:~/$REMOTE/repo/tests/fixtures/"
rsync -a "$SPV_DIR/" "$DOCKER_HOST:~/$REMOTE/spv/"

# Run the build + test inside ubuntu:24.04.
ssh "$DOCKER_HOST" bash -s <<REMOTE_SH
set -euo pipefail
cd ~/$REMOTE
sudo docker run --rm -v "\$PWD:/work" -w /work/repo ubuntu:24.04 bash -ceu '
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq >/dev/null
  apt-get install -y --no-install-recommends \
    build-essential meson ninja-build pkg-config \
    libvulkan-dev mesa-vulkan-drivers vulkan-tools \
    python3 >/dev/null 2>&1
  meson setup build-linux --buildtype=release >/dev/null
  meson compile -C build-linux triangle-lavapipe-e2e 2>&1 | tail -15
  export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
  export LAGFX_TRIANGLE_SPV_DIR=/work/spv
  ./build-linux/tests/triangle-lavapipe-e2e
'
REMOTE_SH

rc=$?

# Pull logs back (nothing to pull, but clean up).
ssh "$DOCKER_HOST" "rm -rf ~/$REMOTE"

exit "$rc"

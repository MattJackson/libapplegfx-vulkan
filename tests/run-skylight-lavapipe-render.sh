#!/usr/bin/env bash
# tests/run-skylight-lavapipe-render.sh
# libapplegfx-vulkan — "beyond Stage 80" gate driver.
#
# Stages the repo on an ssh-reachable Docker host with lavapipe, builds the
# air-translate tool + the skylight-lavapipe-render harness, translates a
# real SkyLight fragment shader with OUR air2spv pipeline, compiles the
# reference passthrough vertex with glslangValidator, and renders the pair
# on lavapipe — asserting the correct colour comes out.
#
# Env: LAGFX_DOCKER_HOST (default: docker)
#      SKYLIGHT_FN (default: SimpleColorFragment)
# Exit 0 = correct pixel; non-zero = fail.

set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
DOCKER_HOST="${LAGFX_DOCKER_HOST:-docker}"
FN="${SKYLIGHT_FN:-SimpleColorFragment}"
REMOTE="lagfx-skyrender-$$"

log() { printf '[skylight-render] %s\n' "$*" >&2; }

log "staging repo to $DOCKER_HOST:~/$REMOTE"
ssh "$DOCKER_HOST" "rm -rf ~/$REMOTE && mkdir -p ~/$REMOTE"
rsync -a --delete --exclude 'build*' --exclude '.git' --exclude 'builddir*' \
  "$REPO/" "$DOCKER_HOST:~/$REMOTE/repo/"

ssh "$DOCKER_HOST" bash -s "$REMOTE" "$FN" <<'REMOTE_SH'
set -euo pipefail
REMOTE="$1"; FN="$2"
cd ~/"$REMOTE/repo"
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json

# 1. Build the translator + extractor (Linux release).
meson setup build-skyrender --buildtype=release >/dev/null 2>&1 || true
ninja -C build-skyrender examples/air-translate examples/triangle-extract-retarget >/dev/null

# 2. Extract + translate the real SkyLight fragment.
mkdir -p /tmp/skyr
build-skyrender/examples/triangle-extract-retarget \
  scratch/skylight-re/SkyLightShaders.air64.metallib /tmp/skyr/ >/dev/null
build-skyrender/examples/air-translate /tmp/skyr/"$FN".bc /tmp/skyr/frag.spv
spirv-val /tmp/skyr/frag.spv

# 3. Compile the reference passthrough vertex.
glslangValidator -V tests/fixtures/skylight_passthrough.vert -o /tmp/skyr/vert.spv >/dev/null

# 4. Build + run the render harness.
cc -O2 tests/skylight-lavapipe-render.c -lvulkan -o /tmp/skyr/render
LAGFX_FORCE_LAVAPIPE=1 /tmp/skyr/render /tmp/skyr/vert.spv /tmp/skyr/frag.spv 00ff00ff
RC=$?
rm -rf ~/"$REMOTE"
exit $RC
REMOTE_SH

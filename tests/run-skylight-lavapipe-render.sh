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

# 2. Extract all SkyLight functions + build the render harness once.
mkdir -p /tmp/skyr
build-skyrender/examples/triangle-extract-retarget \
  scratch/skylight-re/SkyLightShaders.air64.metallib /tmp/skyr/ >/dev/null
cc -O2 tests/skylight-lavapipe-render.c -lvulkan -o /tmp/skyr/render
glslangValidator -V tests/fixtures/skylight_passthrough.vert -o /tmp/skyr/vert_pass.spv >/dev/null
glslangValidator -V tests/fixtures/skylight_texcoord.vert  -o /tmp/skyr/vert_tex.spv  >/dev/null

RC=0

# Case A: passthrough colour fragment (no resources).
echo "=== passthrough: $FN ==="
build-skyrender/examples/air-translate /tmp/skyr/"$FN".bc /tmp/skyr/frag.spv
spirv-val /tmp/skyr/frag.spv
LAGFX_FORCE_LAVAPIPE=1 /tmp/skyr/render /tmp/skyr/vert_pass.spv /tmp/skyr/frag.spv 00ff00ff || RC=1

# Case B: real texture-sampling fragment with a bound texture+sampler.
TEXFN="${SKYLIGHT_TEXFN:-SimpleTextureFragment}"
echo "=== texture-sample: $TEXFN ==="
build-skyrender/examples/air-translate /tmp/skyr/"$TEXFN".bc /tmp/skyr/texfrag.spv
spirv-val /tmp/skyr/texfrag.spv
LAGFX_FORCE_LAVAPIPE=1 /tmp/skyr/render /tmp/skyr/vert_tex.spv /tmp/skyr/texfrag.spv 0000ffff --tex || RC=1

# Case C: REAL live-guest ColorFill fragment reading a vec4 from a
# StorageBuffer (set 0, binding 0) — proves the [[buffer]] descriptor path
# (reflection-shaped layout + bound storage buffer -> shader reads it ->
# correct pixel) end-to-end on the production lavapipe stack.
echo "=== storage-buffer: guest ColorFill ==="
build-skyrender/examples/air-translate tests/fixtures/guest_colorfill_frag.air.bc /tmp/skyr/buffrag.spv
spirv-val /tmp/skyr/buffrag.spv
LAGFX_FORCE_LAVAPIPE=1 /tmp/skyr/render /tmp/skyr/vert_tex.spv /tmp/skyr/buffrag.spv 00ccffff --buf || RC=1

rm -rf ~/"$REMOTE"
exit $RC
REMOTE_SH

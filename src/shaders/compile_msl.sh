#!/usr/bin/env bash
# libapplegfx-vulkan — MSL -> AIR -> metallib -> SPIR-V pipeline driver
# src/shaders/compile_msl.sh
#
# Copyright © 2026 Matthew Jackson
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Takes every src/shaders/msl/*.metal source file and runs it through
# the full Phase 3.C.2 Apple-AIR pipeline:
#
#   (1) xcrun metal -c              — MSL -> .air                (Darwin)
#   (2) xcrun metallib              — .air -> .metallib          (Darwin)
#   (3) triangle-extract-retarget   — metallib -> per-fn .bc     (our lib)
#   (4) llc -mtriple=spirv-*        — .bc -> .raw.spv            (LLVM 20)
#   (5) triangle-spv-rewrite        — signature-transform .raw.spv -> .spv
#
# Stages (1)-(3) + (5) run locally on the Mac build host (they need
# xcrun + our meson build). Stage (4) requires LLVM 20 with the SPIR-V
# backend, which is only packaged on Linux today — we shell out to a
# docker host via ssh for that step, mirroring
# examples/triangle/build_spirv.sh. Set LAGFX_DOCKER_HOST to change
# the ssh target (default: `docker`).
#
# Output: src/shaders/spv-apple/<entry>.spv — one SPIR-V blob per
# (shader, stage) pair. The names match the MSL function names
# (e.g. blit_vertex.spv, blit_fragment.spv) so downstream consumers
# (tests/apple-stock-shaders.c, the meson custom_target) can
# enumerate them without a manifest.
#
# === M5 "prove Apple-AIR pipeline works for the stock shaders" ===
#
# Goal: the stock shaders must be reachable via the Apple-authored
# MSL -> AIR -> SPIR-V path so we have end-to-end confidence in the
# translator, not just the GLSL reference bypass. The .spv blobs
# produced here are SHIPPED in src/shaders/spv-apple/ alongside the
# GLSL-sourced src/shaders/spv/ twins so:
#
#   - Linux CI runs apple-stock-shaders.c against them (validates
#     lavapipe vkCreateShaderModule acceptance).
#   - Mac contributors can refresh them by re-running this script
#     whenever the MSL sources change.
#
# Usage:
#   src/shaders/compile_msl.sh                 # -> src/shaders/spv-apple/
#   src/shaders/compile_msl.sh <out-dir>       # -> <out-dir>/
#   SKIP_DOCKER=1 src/shaders/compile_msl.sh   # emit .bc only (stage 1-3)
#
# Env:
#   LAGFX_BUILD        meson build dir (default: build at repo root)
#   LAGFX_DOCKER_HOST  ssh host that runs docker (default: docker)
#   SKIP_DOCKER=1      stop after stage 3 (.bc files only)
#   KEEP_INTERMEDIATES keep .air/.metallib/.bc/.raw.spv in <out-dir>
#
# Exits non-zero on any stage failure.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../.." && pwd)"
msl_dir="${script_dir}/msl"

# Default output lands in src/shaders/spv-apple/ so the emitted
# blobs are picked up by meson.build's shipping rule. Call sites
# that want a scratch directory pass one explicitly.
out_dir="${1:-${script_dir}/spv-apple}"

build_dir="${LAGFX_BUILD:-${repo_root}/build}"
docker_host="${LAGFX_DOCKER_HOST:-docker}"

log() { printf '[compile_msl] %s\n' "$*" >&2; }

# --- Host preflight -------------------------------------------------------

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "compile_msl.sh: requires macOS for stages 1-2 (got $(uname -s))" >&2
    echo "  install Xcode Command Line Tools + the Metal toolchain:" >&2
    echo "    xcode-select --install" >&2
    echo "    xcodebuild -downloadComponent MetalToolchain" >&2
    exit 2
fi

if ! command -v xcrun >/dev/null 2>&1; then
    echo "compile_msl.sh: xcrun not on PATH — install Xcode CLT with" >&2
    echo "  xcode-select --install" >&2
    exit 2
fi

# xcrun returns 72 with "missing Metal Toolchain" if the toolchain
# is not installed. We prefer the dedicated --version probe so the
# user sees the exact version that'll drive the compile.
if ! xcrun -sdk macosx metal --version >/dev/null 2>&1; then
    echo "compile_msl.sh: Metal toolchain missing; install via" >&2
    echo "  xcodebuild -downloadComponent MetalToolchain" >&2
    exit 3
fi

if [[ ! -d "${msl_dir}" ]]; then
    echo "compile_msl.sh: no MSL sources at ${msl_dir}" >&2
    echo "  author <name>.metal files there (one per shader)" >&2
    exit 2
fi

# --- Stage 0: ensure our lib's CLI tools are built ------------------------
#
# triangle-extract-retarget and triangle-spv-rewrite live under
# examples/ and build_by_default:true, so a fresh `meson compile`
# in an existing build dir is enough. We only set up the build dir
# if it doesn't exist yet.

extract_bin="${build_dir}/examples/triangle-extract-retarget"
rewrite_bin="${build_dir}/examples/triangle-spv-rewrite"

if [[ ! -x "${extract_bin}" || ! -x "${rewrite_bin}" ]]; then
    log "Stage 0: building triangle-extract-retarget + triangle-spv-rewrite"
    if [[ ! -d "${build_dir}" ]]; then
        meson setup "${build_dir}" "${repo_root}"
    fi
    meson compile -C "${build_dir}" triangle-extract-retarget triangle-spv-rewrite
fi

mkdir -p "${out_dir}"

# Intermediates live in <out-dir>/.intermediates/ so spv-apple/ only
# contains the final .spv files. The KEEP_INTERMEDIATES env flag
# leaves them around for triage.
int_dir="${out_dir}/.intermediates"
mkdir -p "${int_dir}"

# --- Stages 1-3: MSL -> .air -> metallib -> retargeted .bc ---------------

bc_files=()
shader_bases=()
for metal_src in "${msl_dir}"/*.metal; do
    base="$(basename "${metal_src}" .metal)"
    shader_bases+=("${base}")

    air_out="${int_dir}/${base}.air"
    lib_out="${int_dir}/${base}.metallib"

    log "Stage 1: ${metal_src} -> ${air_out}"
    xcrun -sdk macosx metal \
        -c \
        -target air64-apple-macos26.3 \
        -o "${air_out}" \
        "${metal_src}"

    log "Stage 2: ${air_out} -> ${lib_out}"
    xcrun -sdk macosx metallib -o "${lib_out}" "${air_out}"

    log "Stage 3: ${lib_out} -> ${int_dir}/${base}_*.bc"
    "${extract_bin}" "${lib_out}" "${int_dir}" >/dev/null

    # The extract tool writes per-function .bc files named after the
    # metallib function names. For our MSL convention the names are
    # <base>_vertex.bc + <base>_fragment.bc.
    for stage in vertex fragment; do
        bc="${int_dir}/${base}_${stage}.bc"
        if [[ ! -f "${bc}" ]]; then
            echo "compile_msl.sh: expected ${bc} from metallib extract" >&2
            exit 4
        fi
        bc_files+=("${bc}")
    done
done

log "Stages 1-3 OK: produced ${#bc_files[@]} per-function .bc blob(s)"

# --- Stage 4: .bc -> .raw.spv (LLVM SPIR-V backend via docker) ----------

if [[ "${SKIP_DOCKER:-0}" == "1" ]]; then
    log "SKIP_DOCKER=1 — stopping after stage 3 (.bc in ${int_dir})"
    exit 0
fi

if ! ssh -o BatchMode=yes -o ConnectTimeout=5 "${docker_host}" true 2>/dev/null; then
    echo "compile_msl.sh: cannot ssh to '${docker_host}' for llc step" >&2
    echo "  set LAGFX_DOCKER_HOST to a reachable docker-capable host," >&2
    echo "  or re-run with SKIP_DOCKER=1 to keep .bc files only" >&2
    exit 5
fi

remote_dir="lagfx-apple-shaders-$(date +%s)"
log "Stage 4: llc -mtriple=spirv-unknown-vulkan1.3 via ${docker_host}:~/${remote_dir}"
ssh "${docker_host}" "mkdir -p ~/${remote_dir}"
scp -q "${bc_files[@]}" "${docker_host}:~/${remote_dir}/"

# The remote bash block installs llvm-20-tools (once per run, cached
# by apt inside the image) and runs llc on every .bc. We capture
# stderr per-file but continue on failure — a single shader hitting
# an llc bug shouldn't halt the whole catalog.
ssh "${docker_host}" "sudo docker run --rm -v ~/${remote_dir}:/work ubuntu:24.04 bash -ceu '
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq >/dev/null
  apt-get install -y --no-install-recommends llvm-20 llvm-20-tools spirv-tools >/dev/null 2>&1
  cd /work
  for bc in *.bc; do
    name=\${bc%.bc}
    if /usr/lib/llvm-20/bin/llc -mtriple=spirv-unknown-vulkan1.3 -filetype=obj \"\$bc\" -o \"\$name.raw.spv\" 2>\"\$name.llc.log\"; then
      printf \"   llc %-30s -> %d bytes\\n\" \"\$bc\" \$(stat -c %s \$name.raw.spv)
    else
      printf \"   llc %-30s -> FAILED (see \$name.llc.log)\\n\" \"\$bc\"
    fi
  done
  chmod -R a+rw /work
'" | sed 's/^/[compile_msl] /' >&2

# Fetch every .raw.spv + .llc.log back. scp globs work on remote
# paths so this grabs all of them in one shot.
scp -q "${docker_host}:~/${remote_dir}/*.raw.spv" "${int_dir}/" 2>/dev/null || true
scp -q "${docker_host}:~/${remote_dir}/*.llc.log" "${int_dir}/" 2>/dev/null || true
ssh "${docker_host}" "rm -rf ~/${remote_dir}"

# --- Stage 5: signature-transform .raw.spv -> .spv -----------------------

xform_ok=()
xform_fail=()
for bc in "${bc_files[@]}"; do
    entry="$(basename "${bc}" .bc)"
    raw="${int_dir}/${entry}.raw.spv"
    final="${out_dir}/${entry}.spv"

    if [[ ! -f "${raw}" ]]; then
        log "Stage 5: ${entry} — no raw.spv (llc did not produce output)"
        xform_fail+=("${entry} [llc failed]")
        continue
    fi

    # Detect stage from the function name suffix. Our MSL convention
    # is <base>_vertex / <base>_fragment — matches triangle_vertex /
    # triangle_fragment in the triangle example.
    case "${entry}" in
        *_vertex)   stage="vertex"   ;;
        *_fragment) stage="fragment" ;;
        *)
            log "Stage 5: ${entry} — cannot infer stage from name"
            xform_fail+=("${entry} [unknown stage]")
            continue
            ;;
    esac

    if "${rewrite_bin}" "${raw}" "${final}" "${entry}" "${stage}" >"${int_dir}/${entry}.xform.log" 2>&1; then
        log "Stage 5: ${entry} -> ${final} ($(wc -c <"${final}") bytes)"
        xform_ok+=("${entry}")
    else
        log "Stage 5: ${entry} — signature-transform FAILED (see ${int_dir}/${entry}.xform.log)"
        xform_fail+=("${entry} [signature-transform failed]")
    fi
done

# --- Summary --------------------------------------------------------------

log "=== SUMMARY ==="
log "transformed OK: ${#xform_ok[@]}"
for e in "${xform_ok[@]}"; do
    log "  + ${e}"
done
if [[ ${#xform_fail[@]} -gt 0 ]]; then
    log "transform FAIL: ${#xform_fail[@]}"
    for e in "${xform_fail[@]}"; do
        log "  - ${e}"
    done
fi

if [[ -z "${KEEP_INTERMEDIATES:-}" ]]; then
    rm -rf "${int_dir}"
fi

if [[ ${#xform_fail[@]} -gt 0 ]]; then
    exit 6
fi

log "OK — ${#xform_ok[@]} .spv blob(s) in ${out_dir}/"

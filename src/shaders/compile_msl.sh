#!/usr/bin/env bash
# libapplegfx-vulkan — MSL → AIR → metallib build-time compile (stub)
# src/shaders/compile_msl.sh
#
# Copyright © 2026 Matthew Jackson
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Compiles each src/shaders/msl/*.metal into an .air object and
# packs them into a single shader_catalog.metallib. Intended to
# run on macOS build hosts only — the Metal toolchain is
# macOS-exclusive and not all contributor / CI hosts have it
# installed.
#
# === Phase 3.C scaffold status ==================================
#
# STUB. This script is NOT wired to the meson build today. The
# generated .air + .metallib artefacts are not yet consumed by
# anything in the library — the catalog is keyed on
# lagfx_shader_kind_t enum values, not on AIR-byte hashes. Once
# Phase 3.C.2 flips the key to SHA-256-64 of AIR (per
# paravirt-re/shader-catalog-plan.md §5) this script becomes the
# build-time producer of those hashes.
#
# Run manually on a macOS host:
#
#   $ src/shaders/compile_msl.sh build/shaders
#
# which emits build/shaders/<name>.air + build/shaders/
# shader_catalog.metallib.
#
# === Host requirements (shader-catalog-plan.md §3.1) ============
#
#   macOS 26.3.1+ (or Ventura-era hosts if you pin -target older)
#   Xcode 26.4.1 (Build 17E202), SDK macosx 26.4
#   Metal Toolchain (xcodebuild -downloadComponent MetalToolchain)
#
# If the toolchain is absent, `xcrun -sdk macosx metal --version`
# prints "missing Metal Toolchain". Install with the xcodebuild
# command above (one-time).
#
# TODO(phase-3c.2): wire this into meson.build under an
#   if host_machine.system() == 'darwin' and metal_toolchain_found
# guard. Emit build/shaders/shader_catalog_hashes.inc for
# catalog.c to pick up AIR-byte hashes. Requires the catalog-refresh
# workflow documented in shader-catalog-plan.md §3.3 — which is
# *not* part of the Phase 3.C scaffold.

set -euo pipefail

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "compile_msl.sh: requires macOS (got $(uname -s))" >&2
    exit 2
fi

if [[ $# -lt 1 ]]; then
    echo "usage: compile_msl.sh <out-dir>" >&2
    exit 2
fi

out_dir="$1"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
msl_dir="${script_dir}/msl"

if [[ ! -d "${msl_dir}" ]]; then
    echo "compile_msl.sh: no MSL sources at ${msl_dir}" >&2
    exit 2
fi

# Verify the Metal toolchain is actually present — xcrun will
# return 72 with a helpful message otherwise.
if ! xcrun -sdk macosx metal --version >/dev/null 2>&1; then
    echo "compile_msl.sh: Metal toolchain missing; install via" >&2
    echo "  xcodebuild -downloadComponent MetalToolchain" >&2
    exit 3
fi

mkdir -p "${out_dir}"

air_files=()
for metal_src in "${msl_dir}"/*.metal; do
    base="$(basename "${metal_src}" .metal)"
    air_out="${out_dir}/${base}.air"
    echo "compile_msl.sh: ${metal_src} -> ${air_out}"
    xcrun -sdk macosx metal \
        -c \
        -target air64-apple-macos26.3 \
        -o "${air_out}" \
        "${metal_src}"
    air_files+=("${air_out}")
done

metallib_out="${out_dir}/shader_catalog.metallib"
echo "compile_msl.sh: packing $(printf '%d' "${#air_files[@]}") .air files into ${metallib_out}"
xcrun -sdk macosx metallib -o "${metallib_out}" "${air_files[@]}"

echo "compile_msl.sh: OK — ${metallib_out} ($(wc -c <"${metallib_out}") bytes)"

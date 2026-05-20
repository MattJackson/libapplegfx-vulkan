# examples/meson.build — E2E examples (Phase 3.C.2)
#
# Builds auxiliary tools that drive src/air2spirv end-to-end against
# real metallib fixtures. Not wired to `meson test` — these examples
# are part of the stage-3 (LLVM SPIR-V backend) runbook and are
# invoked by shell scripts that shell out to `xcrun metal` and `llc`.

if fs.is_file(meson.current_source_dir() / 'triangle' / 'extract_retarget.c')
  executable('triangle-extract-retarget',
    'triangle/extract_retarget.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : libapplegfx_vulkan_dep,
    build_by_default : true,
  )
endif

# SPV entry-point rewriter — runs after llc produces SPIR-V from the
# retargeted .bc files. Fixes up OpCapability/OpEntryPoint/CPacked/
# LinkageAttributes so vkCreateShaderModule accepts the result. See
# src/air2spirv/spv_entrypoint_rewrite.c for the full edit list.
if fs.is_file(meson.current_source_dir() / 'triangle' / 'spv_rewrite.c')
  executable('triangle-spv-rewrite',
    'triangle/spv_rewrite.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : libapplegfx_vulkan_dep,
    build_by_default : true,
  )
endif

# Extract-only tool — reads a metallib and writes one .air.bc file per
# function. No retargeting is performed; the AIR bitcode is written as-
# found for offline analysis (e.g., intrinsic cataloguing). See
# triangle/extract_only.c for details.
if fs.is_file(meson.current_source_dir() / 'triangle' / 'extract_only.c')
  executable('triangle-extract-only',
    'triangle/extract_only.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : libapplegfx_vulkan_dep,
    build_by_default : true,
  )
endif

# air-bcdump — bcanalyzer-like structural dump tool that exercises our
# clean-room Phase 1 + Phase 2 decoders. Used by the Phase 2 regression
# harness (paravirt-re/library/diag/air_reader_diff.py) and for ad-hoc
# inspection of captured macOS metallib bodies.
executable('air-bcdump',
  'air-bcdump.c',
  include_directories : [include_dir, internal_include_dir],
  dependencies : libapplegfx_vulkan_dep,
  build_by_default : true,
)

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

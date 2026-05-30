# tests/meson.build — header, memory, lifecycle, and optional vulkan smoke.

# Header syntax check — always runnable; also smoke-tests version.c
# via libapplegfx_vulkan_dep.
header_deps = [libapplegfx_vulkan_dep]
if vulkan_dep.found()
  header_deps += [vulkan_dep]
endif
header_test = executable('header-syntax-check',
  'header-syntax-check.c',
  include_directories : include_dir,
  dependencies : header_deps,
  build_by_default : true,
)
test('header syntax', header_test)

# Lifecycle smoke — covers device + display + mmio stubs.
if fs.is_file(meson.current_source_dir() / 'lifecycle-smoke.c')
  lifecycle_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    lifecycle_deps += [vulkan_dep]
  endif
  lifecycle_test = executable('lifecycle-smoke',
    'lifecycle-smoke.c',
    include_directories : include_dir,
    dependencies : lifecycle_deps,
    build_by_default : true,
  )
  test('lifecycle smoke', lifecycle_test)
endif

# memory-task test (only built when src/memory has produced task.c AND
# the local test source exists). Runtime memfd/MAP_FIXED behavior is
# Linux-only, so we only register the meson test() on Linux hosts;
# on other hosts we still BUILD it to catch syntax regressions.
if fs.is_file(meson.current_source_dir() / 'memory-task.c') and \
   fs.is_file(meson.project_source_root() / 'src' / 'memory' / 'task.c')
  memory_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    memory_deps += [vulkan_dep]
  endif
  memory_test = executable('memory-task-test',
    'memory-task.c',
    include_directories : include_dir,
    dependencies : memory_deps,
    build_by_default : true,
  )
  if host_machine.system() == 'linux'
    test('memory task', memory_test)
  else
    message('memory-task test built but not run (non-Linux host).')
  endif
endif

# memory-coherence test (Phase 1.C gate for Phase 2 first-pixel).
# Proves lagfx_task_map_host_memory aliases host_ptr's pages into the
# task VA rather than copying them. See docs/memory-coherence-audit.md
# and tests/memory-coherence.c for the full rationale.
#
# Linux-only test() registration: the fix uses mremap() which is Linux-
# specific. On Darwin the library intentionally falls back to copy-on-
# map (dev path only, per audit §4), and the test would correctly fail
# there — so we BUILD on Darwin for syntax coverage but only RUN on
# Linux. The test itself exits 77 (meson SKIP) at runtime if the host
# kernel lacks memfd_create, keeping CI green on very old Linux.
if fs.is_file(meson.current_source_dir() / 'memory-coherence.c') and \
   fs.is_file(meson.project_source_root() / 'src' / 'memory' / 'task.c')
  memory_coherence_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    memory_coherence_deps += [vulkan_dep]
  endif
  memory_coherence_test = executable('memory-coherence',
    'memory-coherence.c',
    include_directories : include_dir,
    dependencies : memory_coherence_deps,
    build_by_default : true,
  )
  if host_machine.system() == 'linux'
    test('memory coherence', memory_coherence_test)
  else
    message('memory-coherence test built but not run (non-Linux host; copy-on-map fallback is expected there).')
  endif
endif

# gpu_cores / thread_count env-plumbing test. Verifies that
# lagfx_device_descriptor_t::thread_count is translated into the
# LP_NUM_THREADS env var by lagfx_device_new, per the spec at
# paravirt-re/gpu-cores-implementation-spec.md.
if fs.is_file(meson.current_source_dir() / 'gpu-cores.c')
  gpu_cores_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    gpu_cores_deps += [vulkan_dep]
  endif
  gpu_cores_test = executable('gpu-cores',
    'gpu-cores.c',
    include_directories : include_dir,
    dependencies : gpu_cores_deps,
    build_by_default : true,
  )
  test('gpu cores', gpu_cores_test)
endif

# Resource registry unit tests (standalone — no Vulkan dependency).
# Tests lagfx_resource_register/lookup/unregister/clear_task directly.
if fs.is_file(meson.current_source_dir() / 'resource-unit.c')
  resource_unit_test = executable('resource-unit',
    'resource-unit.c',
    include_directories : [include_dir, internal_include_dir],
    build_by_default : true,
  )
  test('resource registry (unit)', resource_unit_test)
endif

# Stamp advancement unit tests (standalone — no Vulkan dependency).
# Tests lagfx_advance_stamp_cell monotonicity directly with buffer inputs.
if fs.is_file(meson.current_source_dir() / 'stamp-unit.c')
  stamp_unit_test = executable('stamp-unit',
    'stamp-unit.c',
    include_directories : [include_dir, internal_include_dir],
    build_by_default : true,
  )
  test('stamp helpers (unit)', stamp_unit_test)
endif

# Deadlock detection tests. Regression suite for ABBA deadlock between
# WindowServer and DisplayPipe. Tests online event timing (must be IMMEDIATE,
# not deferred), stamp ACK latency (<10ms), and device creation timeout (<5s).
if fs.is_file(meson.current_source_dir() / 'deadlock-detect.c')
  deadlock_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    deadlock_deps += [vulkan_dep]
  endif
  deadlock_detect_test = executable('deadlock-detect',
    'deadlock-detect.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : deadlock_deps,
    build_by_default : true,
  )
  test('deadlock detection', deadlock_detect_test)
endif

# Render opcode tests - drawPrimitives family (most used opcodes).
if fs.is_file(meson.current_source_dir() / 'render-opcode-draw.c')
  render_draw_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    render_draw_deps += [vulkan_dep]
  endif
  render_draw_test = executable('render-opcode-draw',
    'render-opcode-draw.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : render_draw_deps,
    build_by_default : true,
  )
  test('render opcode draw', render_draw_test)
endif

# Render opcode tests - pipeline state setup (setPipelineState, blend constants).
if fs.is_file(meson.current_source_dir() / 'render-opcode-pipeline.c')
  render_pipe_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    render_pipe_deps += [vulkan_dep]
  endif
  render_pipeline_test = executable('render-opcode-pipeline',
    'render-opcode-pipeline.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : render_pipe_deps,
    build_by_default : true,
  )
  test('render opcode pipeline', render_pipeline_test)
endif

# ExecIndirect2 outer-payload parser tests. Validates CmdExecIndirect2
# payload parsing (task_id, descriptor_count, resource_count), query mode
# with minimal payloads, and empty completion handling.
if fs.is_file(meson.current_source_dir() / 'cmdexecindirect2-parser.c')
  execind_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    execind_deps += [vulkan_dep]
  endif
  execind_test = executable('cmdexecindirect2-parser',
    'cmdexecindirect2-parser.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : execind_deps,
    build_by_default : true,
  )
  test('execindirect2 parser', execind_test)
endif

# Shader catalog unit tests - LRU eviction and lookup.
if fs.is_file(meson.current_source_dir() / 'm1-shader-catalog.c')
  shader_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    shader_deps += [vulkan_dep]
  endif
  shader_catalog_test = executable('m1-shader-catalog',
    'm1-shader-catalog.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : shader_deps,
    build_by_default : true,
  )
  test('shader catalog', shader_catalog_test)
endif

# Optional lavapipe smoke test — only built when vulkan is present.
if fs.is_file(meson.current_source_dir() / 'lavapipe-smoke.c') \
   and vulkan_dep.found()
  lavapipe_test = executable('lavapipe-smoke',
    'lavapipe-smoke.c',
    include_directories : include_dir,
    dependencies : [vulkan_dep],
    build_by_default : false,  # long-ish; run manually
  )
  # Not wired to `meson test` auto-run — it requires lavapipe ICD on
  # the host which CI may or may not provide. Run with:
  #   meson compile -C build lavapipe-smoke && ./build/tests/lavapipe-smoke
endif

# Phase 1.B: Vulkan init smoke. Only registered when vulkan_dep is
# present at configure time; the test itself emits SKIP at runtime
# if the host has no loadable ICD (typical on a Darwin dev box —
# the library's LAGFX_HAVE_VULKAN path still falls back to the no-op
# stub init, which the test recognises and skips gracefully).
if fs.is_file(meson.current_source_dir() / 'vulkan-init.c') \
   and vulkan_dep.found()
  vulkan_init_test = executable('vulkan-init',
    'vulkan-init.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep, vulkan_dep],
    build_by_default : true,
  )
  test('vulkan init', vulkan_init_test)
endif

# Phase 1.B.2: Vulkan command pool + empty submit smoke. Same gating
# + runtime SKIP policy as the Phase 1.B init test — depends on the
# vulkan dep at configure time, tolerates the no-loadable-ICD case at
# runtime by emitting SKIP instead of failing.
if fs.is_file(meson.current_source_dir() / 'vulkan-command.c') \
   and vulkan_dep.found()
  vulkan_command_test = executable('vulkan-command',
    'vulkan-command.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep, vulkan_dep],
    build_by_default : true,
  )
  test('vulkan command', vulkan_command_test)
endif

# Phase 2.B: Vulkan render target + clear + readback smoke. Same gating
# + runtime SKIP policy — the test creates a 64x64 BGRA8 render target,
# clears it to red, reads back, and asserts the centre pixel is red.
if fs.is_file(meson.current_source_dir() / 'vulkan-render.c') \
   and vulkan_dep.found()
  vulkan_render_test = executable('vulkan-render',
    'vulkan-render.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep, vulkan_dep],
    build_by_default : true,
  )
  test('vulkan render', vulkan_render_test)
endif

# Phase 3.C scaffold: shader catalog lookup smoke. Exercises the
# static catalog table and SPIR-V-magic asserts on every stock
# shader's vertex + fragment blob. Darwin-compatible — does not
# touch Vulkan at runtime (vkCreateShaderModule lands Phase 3.E);
# the gate is only on the test source existing. Reaches into src/
# for shaders/catalog.h, so it needs internal_include_dir.
if fs.is_file(meson.current_source_dir() / 'shader-catalog.c')
  shader_catalog_test = executable('shader-catalog',
    'shader-catalog.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : libapplegfx_vulkan_dep,
    build_by_default : true,
  )
  test('shader catalog', shader_catalog_test)
endif

# Phase 3.C: Stock-shader VkShaderModule round-trip smoke. Complements
# tests/shader-catalog.c — that test only asserts SPIR-V magic on the
# embedded blobs; this one actually feeds each blob through
# vkCreateShaderModule on a live Vulkan device and asserts the loader
# accepts it. Same gating + runtime-SKIP policy as vulkan-render:
# registered only when vulkan_dep was present at configure time, and
# the test itself emits SKIP (exit 0) when no loadable ICD is present.
# Reaches into src/ for vulkan/instance.h + shaders/catalog.h, so
# internal_include_dir is needed.
if fs.is_file(meson.current_source_dir() / 'stock-shaders.c')
  stock_shaders_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    stock_shaders_deps += [vulkan_dep]
  endif
  stock_shaders_test = executable('stock-shaders',
    'stock-shaders.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : stock_shaders_deps,
    build_by_default : true,
  )
  test('stock shaders', stock_shaders_test)
endif

# Phase 3.C.2 M5: Apple-pipeline stock-shader lavapipe load smoke.
# Parallel to stock-shaders above, but exercises the Apple-AIR-sourced
# SPV blobs shipped at src/shaders/spv-apple/ (produced by src/
# shaders/compile_msl.sh). Point of the test is to prove the stock
# shaders ALSO reach lavapipe via the real Apple translation pipeline
# (MSL -> AIR -> metallib -> llc -> sig-xform -> SPIR-V), not just the
# GLSL reference bypass — the M5 full-completeness bar.
#
# Uses LAGFX_APPLE_SPV_BUILD_DIR (compile-time define, populated from
# src/shaders/meson.build's apple_shader_spv_build_dir variable) to
# locate the blobs staged into the build tree via configure_file(
# copy:true). Runtime env LAGFX_APPLE_SPV_DIR overrides.
#
# Declares apple_spv_files as additional sources so meson retriggers
# the test when any apple_*.spv changes — same pattern as
# catalog_embedded for the GLSL path.
if fs.is_file(meson.current_source_dir() / 'apple-stock-shaders.c')
  apple_stock_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    apple_stock_deps += [vulkan_dep]
  endif
  apple_stock_cargs = [
    '-DLAGFX_APPLE_SPV_BUILD_DIR="' + apple_shader_spv_build_dir + '"',
  ]
  apple_stock_test = executable('apple-stock-shaders',
    'apple-stock-shaders.c',
    include_directories : [include_dir],
    dependencies : apple_stock_deps,
    c_args : apple_stock_cargs,
    build_by_default : true,
  )
  # The apple_spv_files are staged at configure time via configure_file(
  # copy:true) — they already exist by the time this test runs, so no
  # explicit test() depends entry is required. A `meson setup
  # --reconfigure` after running src/shaders/compile_msl.sh is enough
  # to pick up refreshed blobs.
  test('apple stock shaders', apple_stock_test)
endif

# Phase 3.A: Metal-to-Vulkan render encoder smoke. The state-
# machine pass runs on every host; the real-Vulkan record path
# self-skips when no loadable ICD is present. When vulkan_dep is
# not present at configure time the test still builds (it exercises
# the LAGFX_ERR_BACKEND stub path) — hence no vulkan_dep gate here.
# Reaches into src/ for translate/render_encoder.h and the vulkan
# helpers, so it needs internal_include_dir.
if fs.is_file(meson.current_source_dir() / 'translate-render.c')
  translate_render_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    translate_render_deps += [vulkan_dep]
  endif
  translate_render_test = executable('translate-render',
    'translate-render.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : translate_render_deps,
    build_by_default : true,
  )
  test('translate render', translate_render_test)
endif

# Phase 3.C.2 E2E: triangle-lavapipe-e2e. Loads SPIR-V blobs produced by
# examples/triangle/build_spirv.sh (MSL -> metallib -> extract -> retarget
# -> llc + GLSL reference pair for the fallback path) and draws a triangle
# via lavapipe. Gated on vulkan_dep + the test source being present; the
# test itself SKIPs at runtime (exit 77) when LAGFX_TRIANGLE_SPV_DIR is
# not populated, which keeps the Mac dev box (no lavapipe ICD, no llc)
# green.
if fs.is_file(meson.current_source_dir() / 'triangle-lavapipe-e2e.c') \
   and vulkan_dep.found()
  triangle_e2e_test = executable('triangle-lavapipe-e2e',
    'triangle-lavapipe-e2e.c',
    include_directories : include_dir,
    dependencies : [vulkan_dep],
    build_by_default : true,
  )
  test('triangle lavapipe e2e', triangle_e2e_test)
endif

# Phase 3.C.2: air2spirv (metallib extraction + LLVM Bitcode
# target-triple retarget) smoke. In-process test — exercises the
# parser against both synthesised MTLB fixtures AND a real-bytes
# `default.metallib` copy (when fixtures/default.metallib exists at
# configure time). The real fixture ships at tests/fixtures/ so the
# Linux + Darwin test runs can validate that our MTLB walker
# agrees with Apple's actual container layout (u16 tag lengths,
# entry-size-prefixed records, stage enum 0/1/2 = vertex/fragment/
# kernel) — the kind of format details a synthesised fixture would
# never surface.
# Reaches into src/ for air2spirv/*.h, so it needs internal_include_dir.
if fs.is_file(meson.current_source_dir() / 'air2spirv.c')
  air2spirv_args = [
    '-DLAGFX_TEST_FIXTURE_DIR="' + (meson.current_source_dir() / 'fixtures') + '"',
  ]
  if fs.is_file(meson.current_source_dir() / 'fixtures' / 'default.metallib')
    air2spirv_args += [ '-DLAGFX_HAVE_REAL_METALLIB=1' ]
  endif
  air2spirv_test = executable('air2spirv',
    'air2spirv.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : libapplegfx_vulkan_dep,
    c_args : air2spirv_args,
    build_by_default : true,
  )
  test('air2spirv', air2spirv_test)
endif


# Page-walked translation regression test for commit 3d9cf76.
if fs.is_file(meson.current_source_dir() / 'exec-cmdbuf-translation.c')
  exec_cmdbuf_translation_test = executable('exec-cmdbuf-translation',
    'exec-cmdbuf-translation.c',
    include_directories : [include_dir, internal_include_dir],
    build_by_default : true,
  )
  test('page-walked translation regression (unit)', exec_cmdbuf_translation_test)
endif

# InfoDecoder reply unit tests. Validates 0x1c2 threadExecutionWidth invariant,
# 0x1c3 NSUInteger long-form fields, and 0x1c6/0x1c7 coordinate pass-through.
if fs.is_file(meson.current_source_dir() / 'info-replies-unit.c')
  info_replies_unit_test = executable('info-replies-unit',
    'info-replies-unit.c',
    include_directories : [include_dir, internal_include_dir],
    build_by_default : true,
  )
  test('InfoDecoder replies (unit)', info_replies_unit_test)
endif

# Render inner-opcode unit tests. Validates 0x1a DescribeRenderPass parse-and-count,
# 0x65 SetBlendColor float field parsing, and 0x74 SetRenderPipelineState reference resolution.
if fs.is_file(meson.current_source_dir() / 'render-inner-ops-unit.c')
  render_inner_ops_unit_test = executable('render-inner-ops-unit',
    'render-inner-ops-unit.c',
    include_directories : [include_dir, internal_include_dir],
    build_by_default : true,
  )
  test('Render inner ops (unit)', render_inner_ops_unit_test)
endif

# Blit inner-opcode unit tests. Validates 0x12f CopyFromTextureToTexture parse and
# 0x141 FillTextureWithColor RGBA64 color field parsing.
if fs.is_file(meson.current_source_dir() / 'blit-inner-ops-unit.c')
  blit_inner_ops_unit_test = executable('blit-inner-ops-unit',
    'blit-inner-ops-unit.c',
    include_directories : [include_dir, internal_include_dir],
    build_by_default : true,
  )
  test('Blit inner ops (unit)', blit_inner_ops_unit_test)
endif

# Phase 3.C.2: shader_translate unit tests. Exercises lagfx_shader_translate_run()
# end-to-end against triangle.metallib fixture. Gated on llc availability at runtime;
# if llc missing, vertex/fragment tests skip (exit 77), other tests still pass.
if fs.is_file(meson.current_source_dir() / 'shader_translate_test.c')
   shader_translate_deps = [libapplegfx_vulkan_dep]
   if vulkan_dep.found()
     shader_translate_deps += [vulkan_dep]
   endif
   shader_translate_test = executable('shader-translate-test',
     'shader_translate_test.c',
     include_directories : [include_dir, internal_include_dir],
     dependencies : shader_translate_deps,
     build_by_default : true,
   )
  test('shader translate (unit)', shader_translate_test,
         workdir : meson.project_source_root())
endif

# Phase 70e: lagfx_pipeline_build unit tests. Exercises pipeline build from
# triangle.metallib shaders via shader_translate -> VkShaderModule -> pipeline.
if fs.is_file(meson.current_source_dir() / 'pipeline_build_test.c')
  pipeline_build_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    pipeline_build_deps += [vulkan_dep]
  endif
  pipeline_build_test = executable('pipeline-build-test',
    'pipeline_build_test.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : pipeline_build_deps,
    build_by_default : true,
  )
  test('pipeline build (unit)', pipeline_build_test,
       workdir : meson.project_source_root())
endif


# Phase 65e: shader_cache unit tests. Exercises LRU eviction, hit/miss stats, and
# stage-distinguishing key generation for the content-addressed SPIR-V cache.
if fs.is_file(meson.current_source_dir() / 'shader_cache_test.c')
    shader_cache_deps = [libapplegfx_vulkan_dep]
    if vulkan_dep.found()
      shader_cache_deps += [vulkan_dep]
    endif
    shader_cache_test = executable('shader-cache-test',
      'shader_cache_test.c',
      include_directories : [include_dir, internal_include_dir],
      dependencies : shader_cache_deps,
      build_by_default : true,
    )
    test('shader cache (unit)', shader_cache_test)
endif

# Phase B object resolver test: empirical validation via LAGFX_PHASE_B_LOOKUP=1
# deploy on phase4 instead of a unit test. The freshman's draft unit test had
# multiple bugs and was removed; the API is correctness-validated by the
# in-vivo MTLB hits documented in phase_b_step5_v2_4_MTLB_CONFIRMED.md.

# Phase 1 clean-room AIR bitcode reader unit tests. Validates wrapper-header
# parsing + invalid-input rejection. Per-block decoder tests land as the
# decoders themselves do (TYPE_BLOCK, PARAMATTR, CONSTANTS, METADATA, etc.).
if fs.is_file(meson.current_source_dir() / 'air-bitcode-reader-unit.c')
  air_reader_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air_reader_deps += [vulkan_dep]
  endif
  air_reader_test = executable('air-bitcode-reader-unit',
    'air-bitcode-reader-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air_reader_deps,
    build_by_default : true,
  )
  test('air bitcode reader (unit)', air_reader_test)
endif

# Phase 3 — METADATA_BLOCK walker. Asserts byte-exact counts +
# specific records against `llvm-bcanalyzer --dump` on triangle.
if fs.is_file(meson.current_source_dir() / 'air-bitcode-reader-metadata-unit.c')
  air_md_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air_md_deps += [vulkan_dep]
  endif
  air_md_test = executable('air-bitcode-reader-metadata-unit',
    'air-bitcode-reader-metadata-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air_md_deps,
    build_by_default : true,
  )
  test('air bitcode reader metadata (unit)', air_md_test)
endif

# Phase 4 reference SPIR-V emitter — air.position output side.
# Validates the spv_builder scaffolding produces a Vulkan-acceptable
# SPIR-V module with a BuiltIn Position output.
if fs.is_file(meson.current_source_dir() / 'air2spv-position-emit-unit.c')
  air2spv_test_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_test_deps += [vulkan_dep]
  endif
  air2spv_pos_test = executable('air2spv-position-emit-unit',
    'air2spv-position-emit-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_test_deps,
    build_by_default : true,
  )
  test('air2spv position emit (unit)', air2spv_pos_test)
endif

# Phase 4 reference SPIR-V emitter — air.vertex_id input + Position
# output. Validates the input-side builtin pattern + load/convert/
# composite-construct ops.
if fs.is_file(meson.current_source_dir() / 'air2spv-vertex-id-emit-unit.c')
  air2spv_vid_test_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_vid_test_deps += [vulkan_dep]
  endif
  air2spv_vid_test = executable('air2spv-vertex-id-emit-unit',
    'air2spv-vertex-id-emit-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_vid_test_deps,
    build_by_default : true,
  )
  test('air2spv vertex_id emit (unit)', air2spv_vid_test)
endif

# Phase 4 reference SPIR-V emitter — air.render_target (fragment stage).
# Validates the fragment-stage pattern + OpExecutionMode + Location
# decoration (vs BuiltIn for the other two emitters).
if fs.is_file(meson.current_source_dir() / 'air2spv-render-target-emit-unit.c')
  air2spv_rt_test_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_rt_test_deps += [vulkan_dep]
  endif
  air2spv_rt_test = executable('air2spv-render-target-emit-unit',
    'air2spv-render-target-emit-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_rt_test_deps,
    build_by_default : true,
  )
  test('air2spv render_target emit (unit)', air2spv_rt_test)
endif

# Phase 4 reference SPIR-V emitter — air.fragment_input (fragment input).
# Validates fragment-stage INPUT variable with Location decoration +
# OpCompositeExtract/OpCompositeConstruct pattern for vec2->vec4 conversion.
if fs.is_file(meson.current_source_dir() / 'air2spv-fragment-input-emit-unit.c')
  air2spv_fi_test_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_fi_test_deps += [vulkan_dep]
  endif
  air2spv_fi_test = executable('air2spv-fragment-input-emit-unit',
    'air2spv-fragment-input-emit-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_fi_test_deps,
    build_by_default : true,
  )
  test('air2spv fragment_input emit (unit)', air2spv_fi_test)
endif

# Phase 4 reference SPIR-V emitter — Pattern E (Uniform-storage-class
# buffer load). Exercises OpAccessChain + OpLoad on a uniform-buffer
 # variable, OpFMul, OpTypeStruct + DescriptorSet/Binding/Block/Offset
 # decorations.
 if fs.is_file(meson.current_source_dir() / 'air2spv-buffer-load-emit-unit.c')
   air2spv_bl_test_deps = [libapplegfx_vulkan_dep]
   if vulkan_dep.found()
     air2spv_bl_test_deps += [vulkan_dep]
   endif
   air2spv_bl_test = executable('air2spv-buffer-load-emit-unit',
     'air2spv-buffer-load-emit-unit.c',
     include_directories : [include_dir, internal_include_dir],
     dependencies : air2spv_bl_test_deps,
     build_by_default : true,
   )
   test('air2spv buffer_load emit (unit)', air2spv_bl_test)
 endif

 # Phase 4 reference SPIR-V emitter — binary ALU ops (OpFAdd, OpFSub, OpFDiv, OpDot).
 # Exercises chained vec4 arithmetic plus scalar dot product.
 if fs.is_file(meson.current_source_dir() / 'air2spv-alu-binops-emit-unit.c')
   air2spv_ab_test_deps = [libapplegfx_vulkan_dep]
   if vulkan_dep.found()
     air2spv_ab_test_deps += [vulkan_dep]
   endif
   air2spv_ab_test = executable('air2spv-alu-binops-emit-unit',
     'air2spv-alu-binops-emit-unit.c',
     include_directories : [include_dir, internal_include_dir],
     dependencies : air2spv_ab_test_deps,
     build_by_default : true,
   )
   test('air2spv alu_binops emit (unit)', air2spv_ab_test)
 endif

# Phase 4 reference SPIR-V emitter — multi-distinct-float
# OpConstantComposite (clear-color shape).
if fs.is_file(meson.current_source_dir() / 'air2spv-constant-float-emit-unit.c')
  air2spv_cf_test_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_cf_test_deps += [vulkan_dep]
  endif
  air2spv_cf_test = executable('air2spv-constant-float-emit-unit',
    'air2spv-constant-float-emit-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_cf_test_deps,
    build_by_default : true,
  )
  test('air2spv constant_float emit (unit)', air2spv_cf_test)
endif

# Phase 4 reference SPIR-V emitter — Pattern F (separate image +
# sampler texture-sample). OpTypeImage / OpTypeSampler /
# OpTypeSampledImage / OpSampledImage / OpImageSampleImplicitLod.
if fs.is_file(meson.current_source_dir() / 'air2spv-texture-sample-emit-unit.c')
  air2spv_ts_test_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_ts_test_deps += [vulkan_dep]
  endif
  air2spv_ts_test = executable('air2spv-texture-sample-emit-unit',
    'air2spv-texture-sample-emit-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_ts_test_deps,
    build_by_default : true,
  )
  test('air2spv texture_sample emit (unit)', air2spv_ts_test)
endif

# Phase 4 reference SPIR-V emitter — Pattern G (OpExtInst GLSL.std.450).
# Single emitter unlocks every air.* math intrinsic family.
if fs.is_file(meson.current_source_dir() / 'air2spv-extinst-glsl-emit-unit.c')
   air2spv_ext_test_deps = [libapplegfx_vulkan_dep]
   if vulkan_dep.found()
     air2spv_ext_test_deps += [vulkan_dep]
   endif
   air2spv_ext_test = executable('air2spv-extinst-glsl-emit-unit',
     'air2spv-extinst-glsl-emit-unit.c',
     include_directories : [include_dir, internal_include_dir],
     dependencies : air2spv_ext_test_deps,
     build_by_default : true,
   )
   test('air2spv extinst_glsl emit (unit)', air2spv_ext_test)
endif

# Phase 4 reference SPIR-V emitter — unary float ops (OpFNegate + OpExtInst FAbs).
# Demonstrates both core SPIR-V unary and GLSL.std.450 ExtInst patterns.
if fs.is_file(meson.current_source_dir() / 'air2spv-unop-emit-unit.c')
   air2spv_unop_test_deps = [libapplegfx_vulkan_dep]
   if vulkan_dep.found()
     air2spv_unop_test_deps += [vulkan_dep]
   endif
   air2spv_unop_test = executable('air2spv-unop-emit-unit',
     'air2spv-unop-emit-unit.c',
     include_directories : [include_dir, internal_include_dir],
     dependencies : air2spv_unop_test_deps,
     build_by_default : true,
   )
   test('air2spv unop emit (unit)', air2spv_unop_test)
endif

# Phase 4 reference SPIR-V emitter — Pattern H (OpVectorShuffle).
if fs.is_file(meson.current_source_dir() / 'air2spv-vector-shuffle-emit-unit.c')
  air2spv_sh_test_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_sh_test_deps += [vulkan_dep]
  endif
  air2spv_sh_test = executable('air2spv-vector-shuffle-emit-unit',
    'air2spv-vector-shuffle-emit-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_sh_test_deps,
    build_by_default : true,
  )
  test('air2spv vector_shuffle emit (unit)', air2spv_sh_test)
endif

# Phase 4 reference SPIR-V emitter — Pattern I (structured control flow).
# OpSelectionMerge + OpBranchConditional + OpPhi.
if fs.is_file(meson.current_source_dir() / 'air2spv-control-flow-emit-unit.c')
  air2spv_cf2_test_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_cf2_test_deps += [vulkan_dep]
  endif
  air2spv_cf2_test = executable('air2spv-control-flow-emit-unit',
    'air2spv-control-flow-emit-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_cf2_test_deps,
    build_by_default : true,
  )
  test('air2spv control_flow emit (unit)', air2spv_cf2_test)
endif

# Phase 4 reference SPIR-V emitter — Pattern J (structured loop).
# OpLoopMerge + Function-storage OpVariable.
if fs.is_file(meson.current_source_dir() / 'air2spv-loop-emit-unit.c')
  air2spv_lp_test_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_lp_test_deps += [vulkan_dep]
  endif
  air2spv_lp_test = executable('air2spv-loop-emit-unit',
    'air2spv-loop-emit-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_lp_test_deps,
    build_by_default : true,
  )
  test('air2spv loop emit (unit)', air2spv_lp_test)
endif

# Phase 5 — AIR module → SPIR-V translator skeleton.
 if fs.is_file(meson.current_source_dir() / 'air2spv-translate-module-unit.c')
   air2spv_xl_test_deps = [libapplegfx_vulkan_dep]
   if vulkan_dep.found()
     air2spv_xl_test_deps += [vulkan_dep]
   endif
   air2spv_xl_test = executable('air2spv-translate-module-unit',
     'air2spv-translate-module-unit.c',
     include_directories : [include_dir, internal_include_dir],
     dependencies : air2spv_xl_test_deps,
     build_by_default : true,
   )
   test('air2spv translate module (unit)', air2spv_xl_test)
 endif

 # Phase 5 — AIR BINOP + UNOP dispatcher unit test.
 if fs.is_file(meson.current_source_dir() / 'air2spv-translate-function-binop-unop-unit.c')
   air2spv_binop_unop_deps = [libapplegfx_vulkan_dep]
   if vulkan_dep.found()
     air2spv_binop_unop_deps += [vulkan_dep]
   endif
   air2spv_binop_unop_test = executable('air2spv-translate-function-binop-unop-unit',
     'air2spv-translate-function-binop-unop-unit.c',
     include_directories : [include_dir, internal_include_dir],
     dependencies : air2spv_binop_unop_deps,
     build_by_default : true,
   )
   test('air2spv translate function binop unop (unit)', air2spv_binop_unop_test)
 endif

# Compile-time opcode pinning — guards the §3.32.15 relational + §3.32.14
# shift/bitwise opcode constants against the canonical SPIR-V numbers.
if fs.is_file(meson.current_source_dir() / 'air2spv-relational-opcodes-unit.c')
  air2spv_relop_test = executable('air2spv-relational-opcodes-unit',
    'air2spv-relational-opcodes-unit.c',
    include_directories : [include_dir, internal_include_dir],
    build_by_default : true,
  )
  test('air2spv relational opcode pinning (unit)', air2spv_relop_test)
endif

# Blocker A regression: a non-void CALL (texture sample) must be counted
# as a value producer so the downstream FADD resolves float types and
# emits OpFAdd, not OpIAdd. Feeds a real compiled fragment shader fixture
# through the translator and asserts the opcode + spirv-val cleanliness.
if fs.is_file(meson.current_source_dir() / 'air2spv-nonvoid-call-valuenumber-unit.c')
  air2spv_nvcall_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_nvcall_deps += [vulkan_dep]
  endif
  air2spv_nvcall_test = executable('air2spv-nonvoid-call-valuenumber-unit',
    'air2spv-nonvoid-call-valuenumber-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_nvcall_deps,
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv non-void CALL value-numbering (unit)', air2spv_nvcall_test,
    workdir : meson.current_source_dir() / '..')
endif

# SPIR-V §2.4 section-ordering invariant: no OpType*/OpConstant* may
# follow the first OpFunction. Guards against handlers (CMP/SELECT,
# non-void CALL, …) emitting a stray type/constant mid-body.
if fs.is_file(meson.current_source_dir() / 'air2spv-type-section-ordering-unit.c')
  air2spv_secord_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_secord_deps += [vulkan_dep]
  endif
  air2spv_secord_test = executable('air2spv-type-section-ordering-unit',
    'air2spv-type-section-ordering-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_secord_deps,
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv type/constant section ordering (unit)', air2spv_secord_test,
    workdir : meson.current_source_dir() / '..')
endif

# AIR-type-index-0-vs-unknown sentinel: float (type index 0) was read as
# "unknown", mis-dispatching CMP/SELECT/BINOP. Real compositor shader
# (clear vertex) must emit a typed OpSelect, not drop or mis-type it.
if fs.is_file(meson.current_source_dir() / 'air2spv-local-const-type-unit.c')
  air2spv_lconst_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_lconst_deps += [vulkan_dep]
  endif
  air2spv_lconst_test = executable('air2spv-local-const-type-unit',
    'air2spv-local-const-type-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_lconst_deps,
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv local-const operand type (unit)', air2spv_lconst_test,
    workdir : meson.current_source_dir() / '..')
endif

# Vertex-input ABI: stage-in attributes (vec params) must be typed
# Location Input variables, not gl_VertexIndex. Real compositor shader
# (color_fill vertex) feeds an attribute into an OpVectorShuffle.
if fs.is_file(meson.current_source_dir() / 'air2spv-vertex-attribute-input-unit.c')
  air2spv_vattr_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_vattr_deps += [vulkan_dep]
  endif
  air2spv_vattr_test = executable('air2spv-vertex-attribute-input-unit',
    'air2spv-vertex-attribute-input-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_vattr_deps,
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv vertex attribute input (unit)', air2spv_vattr_test,
    workdir : meson.current_source_dir() / '..')
endif

# CALL_FMF (fast-math-flags) operand skip + type-index-0 return type. A
# fast-math conversion CALL must propagate its float result type so a
# downstream float multiply dispatches as OpFMul, not OpIMul.
if fs.is_file(meson.current_source_dir() / 'air2spv-call-fmf-unit.c')
  air2spv_fmf_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_fmf_deps += [vulkan_dep]
  endif
  air2spv_fmf_test = executable('air2spv-call-fmf-unit',
    'air2spv-call-fmf-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_fmf_deps,
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv CALL_FMF operand skip (unit)', air2spv_fmf_test,
    workdir : meson.current_source_dir() / '..')
endif

# Fragment RET → Location-0 colour-output store regression guard.
if fs.is_file(meson.current_source_dir() / 'air2spv-fragment-color-store-unit.c')
  air2spv_fcs_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_fcs_deps += [vulkan_dep]
  endif
  air2spv_fcs_test = executable('air2spv-fragment-color-store-unit',
    'air2spv-fragment-color-store-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_fcs_deps,
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv fragment colour-output store (unit)', air2spv_fcs_test,
    workdir : meson.current_source_dir() / '..')
endif

# air.convert.* numeric-conversion lowering (Stage-80 black-screen guard).
if fs.is_file(meson.current_source_dir() / 'air2spv-air-convert-uitofp-unit.c')
  air2spv_cvt_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_cvt_deps += [vulkan_dep]
  endif
  air2spv_cvt_test = executable('air2spv-air-convert-uitofp-unit',
    'air2spv-air-convert-uitofp-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_cvt_deps,
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv air.convert uitofp lowering (unit)', air2spv_cvt_test,
    workdir : meson.current_source_dir() / '..')
endif

# Real-Apple math-intrinsic naming (air.<op>/air.fast_<op>) lowering guard.
if fs.is_file(meson.current_source_dir() / 'air2spv-air-math-intrinsics-unit.c')
  air2spv_math_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_math_deps += [vulkan_dep]
  endif
  air2spv_math_test = executable('air2spv-air-math-intrinsics-unit',
    'air2spv-air-math-intrinsics-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_math_deps,
    c_args : ['-DSRCDIR="' + meson.current_source_dir() + '"'],
    build_by_default : true,
  )
  test('air2spv real-Apple math intrinsics (unit)', air2spv_math_test,
    workdir : meson.current_source_dir() / '..')
endif

# Phase 4 reference SPIR-V emitter — Pattern K (storage-image OpImageRead / OpImageWrite).
if fs.is_file(meson.current_source_dir() / 'air2spv-storage-image-emit-unit.c')
  air2spv_si_test_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    air2spv_si_test_deps += [vulkan_dep]
  endif
  air2spv_si_test = executable('air2spv-storage-image-emit-unit',
    'air2spv-storage-image-emit-unit.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : air2spv_si_test_deps,
    build_by_default : true,
  )
  test('air2spv storage_image emit (unit)', air2spv_si_test)
endif



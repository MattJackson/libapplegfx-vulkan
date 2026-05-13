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

# Protocol dispatch tests (Phase 1.A.2). Reaches into src/ for
# internal protocol headers, so it needs internal_include_dir too.
if fs.is_file(meson.current_source_dir() / 'protocol-dispatch.c')
  protocol_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    protocol_deps += [vulkan_dep]
  endif
  protocol_dispatch_test = executable('protocol-dispatch',
    'protocol-dispatch.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : protocol_deps,
    build_by_default : true,
  )
  test('protocol dispatch', protocol_dispatch_test)
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

# Resource registry integration tests (requires Vulkan for opcode wiring).
if fs.is_file(meson.current_source_dir() / 'resource-registry.c')
  resource_registry_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    resource_registry_deps += [vulkan_dep]
  endif
  resource_registry_test = executable('resource-registry',
    'resource-registry.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : resource_registry_deps,
    build_by_default : true,
  )
  test('resource registry (integration)', resource_registry_test)
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

# Stamp helper integration tests (requires Vulkan for complete_stamp_slot).
if fs.is_file(meson.current_source_dir() / 'stamp-helpers.c')
  stamp_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    stamp_deps += [vulkan_dep]
  endif
  stamp_helpers_test = executable('stamp-helpers',
    'stamp-helpers.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : stamp_deps,
    build_by_default : true,
  )
  test('stamp helpers (integration)', stamp_helpers_test)
endif

# M4 task-translate + define_host_task unit tests. Covers
# lagfx_task_translate (positive, unknown task, zero root_pfn, zero
# data_pfn, page-cross run_len, read_memory failure) and
# lagfx_op_define_host_task (allocate, update-existing, min-payload).
if fs.is_file(meson.current_source_dir() / 'm4-task-translate.c')
  m4_translate_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    m4_translate_deps += [vulkan_dep]
  endif
  m4_task_translate_test = executable('m4-task-translate',
    'm4-task-translate.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : m4_translate_deps,
    build_by_default : true,
  )
  test('m4 task translate', m4_task_translate_test)
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

# Per-channel doorbell-drain tests (BAR0+0x1020). Exercises the ch=1..4
# ring-walk + dispatch_one_no_stamp branch, the ch=5..7 setupSharedState
# branch, the page0[0] PFN-array indirection vs the +0x1000 fallback,
# and multi-cmd drain w/ last_stamp capture.
if fs.is_file(meson.current_source_dir() / 'doorbell-drain.c')
  doorbell_deps = [libapplegfx_vulkan_dep]
  if vulkan_dep.found()
    doorbell_deps += [vulkan_dep]
  endif
  doorbell_test = executable('doorbell-drain',
    'doorbell-drain.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : doorbell_deps,
    build_by_default : true,
  )
  test('doorbell drain', doorbell_test)
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

# Trace replay harness. Reads a simple text trace (W/R events captured
# from live guest boots) and replays it through the decoder's MMIO
# entry points against a mock device. Regression gate: catches drift
# in mmio_{read,write} behaviour as we iterate on src/protocol/. The
# shipped trace (traces/m2-m3-init-2026-04-21.trace) covers the M2->M3
# init-phase stamps 1-7 pattern. Reaches into src/ for protocol/state.h
# so needs internal_include_dir.
if fs.is_file(meson.current_source_dir() / 'trace-replay.c') \
   and fs.is_file(meson.current_source_dir() / 'traces' / 'm2-m3-init-2026-04-21.trace')
  trace_replay_default = meson.current_source_dir() / 'traces' / 'm2-m3-init-2026-04-21.trace'
  # trace-replay includes state.h which transitively pulls vulkan headers.
  trace_replay_test = executable('trace-replay',
    'trace-replay.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : [libapplegfx_vulkan_dep, vulkan_dep],
    c_args : [ '-DLAGFX_TRACE_REPLAY_DEFAULT="' + trace_replay_default + '"' ],
    build_by_default : true,
  )
  test('trace replay', trace_replay_test,
    args : [ trace_replay_default ])
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

# fuzz-protocol-dispatch — libFuzzer harness for the protocol dispatcher.
# Built only when -Dfuzz=enabled (default disabled). Combine with
# -Db_sanitize=address,undefined for the standard fuzzing build.
#
# Run: ./tests/fuzz-protocol-dispatch -max_total_time=300 path/to/corpus
fuzz_opt = get_option('fuzz')
if fuzz_opt.enabled() and fs.is_file(meson.current_source_dir() / 'fuzz-protocol-dispatch.c')
  cc = meson.get_compiler('c')
  fuzz_link_args = ['-fsanitize=fuzzer']
  fuzz_c_args    = ['-fsanitize=fuzzer-no-link']
  if cc.has_argument('-fsanitize=fuzzer')
    fuzz_target = executable('fuzz-protocol-dispatch',
      'fuzz-protocol-dispatch.c',
      include_directories : [include_dir, internal_include_dir],
      dependencies : libapplegfx_vulkan_dep,
      c_args : fuzz_c_args,
      link_args : fuzz_link_args,
      build_by_default : true,
    )
  else
    warning('fuzz=enabled but compiler does not support -fsanitize=fuzzer; skipping')
  endif
endif

# tests/meson.build — header, memory, lifecycle, and optional vulkan smoke.

# Header syntax check — always runnable; also smoke-tests version.c
# via libapplegfx_vulkan_dep.
header_test = executable('header-syntax-check',
  'header-syntax-check.c',
  include_directories : include_dir,
  dependencies : libapplegfx_vulkan_dep,
  build_by_default : true,
)
test('header syntax', header_test)

# Lifecycle smoke — covers device + display + mmio stubs.
if fs.is_file(meson.current_source_dir() / 'lifecycle-smoke.c')
  lifecycle_test = executable('lifecycle-smoke',
    'lifecycle-smoke.c',
    include_directories : include_dir,
    dependencies : libapplegfx_vulkan_dep,
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
  memory_test = executable('memory-task-test',
    'memory-task.c',
    include_directories : include_dir,
    dependencies : libapplegfx_vulkan_dep,
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
  memory_coherence_test = executable('memory-coherence',
    'memory-coherence.c',
    include_directories : include_dir,
    dependencies : libapplegfx_vulkan_dep,
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
  gpu_cores_test = executable('gpu-cores',
    'gpu-cores.c',
    include_directories : include_dir,
    dependencies : libapplegfx_vulkan_dep,
    build_by_default : true,
  )
  test('gpu cores', gpu_cores_test)
endif

# Protocol dispatch tests (Phase 1.A.2). Reaches into src/ for
# internal protocol headers, so it needs internal_include_dir too.
if fs.is_file(meson.current_source_dir() / 'protocol-dispatch.c')
  protocol_dispatch_test = executable('protocol-dispatch',
    'protocol-dispatch.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : libapplegfx_vulkan_dep,
    build_by_default : true,
  )
  test('protocol dispatch', protocol_dispatch_test)
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

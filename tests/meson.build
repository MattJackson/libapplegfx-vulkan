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

project('libapplegfx-vulkan', 'c',
  version : '0.0.1',
  license : 'AGPL-3.0-or-later',
  default_options : [
    'c_std=c11',
    'warning_level=2',
    'werror=false',         # during dev
    'default_library=shared',
  ],
  meson_version : '>=1.0.0')

cc = meson.get_compiler('c')

add_project_arguments([
  '-D_GNU_SOURCE',          # memfd_create, mmap MAP_FIXED helpers
  '-Wno-unused-parameter',  # many callback stubs have ignored args
], language : 'c')

include_dir = include_directories('include')

fs = import('fs')

# --- Sources --------------------------------------------------------------
# src/ subdirs are populated incrementally by Phase 1 work. Each subdir adds
# its files via subdir('src/foo') which appends to the lagfx_sources list.
lagfx_sources = []

# Populate as subdirs land. Each subdir has its own meson.build that appends
# to lagfx_sources.
if fs.is_file(meson.current_source_dir() / 'src' / 'memory' / 'meson.build')
  subdir('src/memory')
endif

# --- Dependencies ---------------------------------------------------------
vulkan_dep = dependency('vulkan', required : false)
if vulkan_dep.found()
  lagfx_deps = [vulkan_dep]
else
  # Early Phase 1 can build without vulkan (stubbed rendering).
  warning('Vulkan not found — building with stub renderer.')
  lagfx_deps = []
endif

# --- Library --------------------------------------------------------------
if lagfx_sources.length() > 0
  libapplegfx_vulkan = library('applegfx-vulkan',
    lagfx_sources,
    include_directories : include_dir,
    dependencies : lagfx_deps,
    install : true)

  libapplegfx_vulkan_dep = declare_dependency(
    link_with : libapplegfx_vulkan,
    include_directories : include_dir)
else
  # Header-only phase — nothing to link yet.
  libapplegfx_vulkan_dep = declare_dependency(
    include_directories : include_dir)
endif

# --- Public header install -----------------------------------------------
install_headers('include/libapplegfx-vulkan.h')

# --- Tests ---------------------------------------------------------------
subdir('tests')

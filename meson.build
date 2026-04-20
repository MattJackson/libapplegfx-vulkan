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

# Split version string into major/minor/patch for -D flags. meson's
# project(version:) is the single source of truth.
ver_parts = meson.project_version().split('.')
version_major = ver_parts.length() > 0 ? ver_parts[0] : '0'
version_minor = ver_parts.length() > 1 ? ver_parts[1] : '0'
version_patch = ver_parts.length() > 2 ? ver_parts[2] : '0'

# Build info string: version + git hash (if available) + host date.
# vcs_tag runs at configure time; fine for now since we rebuild often.
git_exe = find_program('git', required : false)
if git_exe.found()
  git_hash_result = run_command(git_exe.full_path(),
    '-C', meson.current_source_dir(),
    'rev-parse', '--short=12', 'HEAD',
    check : false)
  if git_hash_result.returncode() == 0
    git_hash = git_hash_result.stdout().strip()
  else
    git_hash = 'unknown'
  endif
else
  git_hash = 'unknown'
endif

build_info_str = '@0@ (git @1@)'.format(meson.project_version(), git_hash)

add_project_arguments([
  '-D_GNU_SOURCE',          # memfd_create, mmap MAP_FIXED helpers
  '-Wno-unused-parameter',  # many callback stubs have ignored args
  '-DLAGFX_VERSION_MAJOR=' + version_major,
  '-DLAGFX_VERSION_MINOR=' + version_minor,
  '-DLAGFX_VERSION_PATCH=' + version_patch,
  '-DLAGFX_BUILD_INFO="' + build_info_str + '"',
], language : 'c')

include_dir = include_directories('include')

# Internal include dir (src/) for the library's own TUs to reach
# device.h / display.h / common/log.h. NOT exposed to consumers.
internal_include_dir = include_directories('src')

fs = import('fs')

# --- Sources --------------------------------------------------------------
# Core library sources live under src/ and are glued together by
# src/meson.build + subdir meson.builds. Each subdir appends to
# lagfx_sources.
lagfx_sources = []

subdir('src')

if fs.is_file(meson.current_source_dir() / 'src' / 'memory' / 'meson.build')
  subdir('src/memory')
endif

# --- Dependencies ---------------------------------------------------------
vulkan_dep = dependency('vulkan', required : false)
if vulkan_dep.found()
  lagfx_deps = [vulkan_dep]
  # Flip the compile-time gate consumed by src/vulkan/instance.c so
  # the real init path (vkCreateInstance, vkCreateDevice, ...) is
  # selected. Without this symbol the file compiles a tiny no-op
  # fallback that still satisfies the lifecycle contract.
  add_project_arguments('-DLAGFX_HAVE_VULKAN=1', language : 'c')
  message('Vulkan found — building real Vulkan init path (Phase 1.B).')
else
  # Early Phase 1 can build without vulkan (stubbed rendering).
  warning('Vulkan not found — building with no-op Vulkan init stubs.')
  lagfx_deps = []
endif

# --- Library --------------------------------------------------------------
libapplegfx_vulkan = library('applegfx-vulkan',
  lagfx_sources,
  include_directories : [include_dir, internal_include_dir],
  dependencies : lagfx_deps,
  version : meson.project_version(),
  soversion : version_major,
  install : true)

libapplegfx_vulkan_dep = declare_dependency(
  link_with : libapplegfx_vulkan,
  include_directories : include_dir)

# pkg-config file so downstream consumers (mos-qemu) can locate us.
# Explicit name/filebase = 'libapplegfx-vulkan' so the resulting .pc file is
# libapplegfx-vulkan.pc (matches QEMU's dependency('libapplegfx-vulkan')).
# Without these overrides meson derives the filename from the library name
# ('applegfx-vulkan'), producing applegfx-vulkan.pc and a silent dependency
# miss in downstream consumers. See M1 dry-run audit (docker-macos/docs).
pkg = import('pkgconfig')
pkg.generate(libapplegfx_vulkan,
  name : 'libapplegfx-vulkan',
  filebase : 'libapplegfx-vulkan',
  description : 'Linux impl of Apple PVG framework (Vulkan backend)',
  url : 'https://github.com/MattJackson/libapplegfx-vulkan',
)

# --- Public header install -----------------------------------------------
install_headers('include/libapplegfx-vulkan.h')

# --- Tests ---------------------------------------------------------------
subdir('tests')

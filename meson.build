project('libapplegfx-vulkan', 'c',
  version : '0.0.1',
  license : 'AGPL-3.0-or-later',
  default_options : ['c_std=c11', 'warning_level=2'])

# Phase 1 will add the actual library. For now, meson just validates
# the public header compiles cleanly.
include_dir = include_directories('include')

# Header-only syntax check target
executable('header-syntax-check',
  'tests/header-syntax-check.c',
  include_directories : include_dir,
  build_by_default : false,
)

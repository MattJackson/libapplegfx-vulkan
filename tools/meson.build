# Standalone tools that don't link against lagfx.
# Python scripts don't need build registration; this file exists
# so `subdir('tools')` works from the top level if needed later.

metallib_dump = executable('metallib_dump',
  ['metallib_dump.c', '../src/metallib/metallib_reader.c'],
  include_directories : [include_dir, internal_include_dir],
  install : false,
  build_by_default : true,
)

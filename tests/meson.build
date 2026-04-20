# Header syntax check — always runnable, even with zero src/.
header_test = executable('header-syntax-check',
  'header-syntax-check.c',
  include_directories : include_dir,
  build_by_default : true,
)
test('header syntax', header_test)

# memory-task test (only built when src/memory has produced task.c AND
# the local test source exists).
# fs is inherited from the parent meson.build scope.
if fs.is_file(meson.current_source_dir() / 'memory-task.c') and \
   fs.is_file(meson.project_source_root() / 'src' / 'memory' / 'task.c')
  memory_test = executable('memory-task-test',
    'memory-task.c',
    include_directories : include_dir,
    dependencies : libapplegfx_vulkan_dep,
    build_by_default : true,
  )
  test('memory task', memory_test)
endif

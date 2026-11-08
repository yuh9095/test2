project(
  'dinit-logind',
  'c',
  version: '0.1.0',
  meson_version: '>= 0.60.0',
  default_options: [
    'c_std=c11',
    'warning_level=3',
    'werror=true',
  ],
)

core_sources = files('src/core/manager.c')

core_lib = static_library(
  'dinit-logind-core',
  core_sources,
  include_directories: include_directories('src'),
  install: false,
)

core_dep = declare_dependency(
  link_with: core_lib,
  include_directories: include_directories('src'),
)

test_exe = executable(
  'test-core',
  'tests/test-core.c',
  dependencies: core_dep,
)

test('core-model', test_exe, timeout: 30)

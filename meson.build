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

udev_dep = dependency('libudev')

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

session_registration_lib = static_library(
  'dinit-logind-session-registration',
  files(
    'src/session/registration.c',
    'src/session/socket.c',
  ),
  dependencies: core_dep,
  include_directories: include_directories('src'),
  install: false,
)

session_registration_dep = declare_dependency(
  link_with: session_registration_lib,
  dependencies: core_dep,
  include_directories: include_directories('src'),
)

linux_platform_lib = static_library(
  'dinit-logind-linux-platform',
  files(
    'src/platform/linux/udev.c',
    'src/platform/linux/udev-manager.c',
    'src/platform/linux/vt.c',
  ),
  dependencies: udev_dep,
  include_directories: include_directories('src'),
  install: false,
)

linux_platform_dep = declare_dependency(
  link_with: linux_platform_lib,
  dependencies: [udev_dep, core_dep],
  include_directories: include_directories('src'),
)

daemon_exe = executable(
  'dinit-logind',
  'src/daemon/main.c',
  dependencies: [linux_platform_dep, session_registration_dep],
  install: true,
)

test(
  'daemon-help',
  daemon_exe,
  args: ['--help'],
  timeout: 10,
)

test(
  'daemon-version',
  daemon_exe,
  args: ['--version'],
  timeout: 10,
)

test_exe = executable(
  'test-core',
  'tests/test-core.c',
  dependencies: core_dep,
)

test('core-model', test_exe, timeout: 30)

registration_test_exe = executable(
  'test-session-registration',
  'tests/test-session-registration.c',
  dependencies: session_registration_dep,
)

test('session-registration', registration_test_exe, timeout: 30)

session_socket_test_exe = executable(
  'test-session-socket',
  'tests/test-session-socket.c',
  dependencies: session_registration_dep,
)

test('session-socket', session_socket_test_exe, timeout: 30)

platform_test_exe = executable(
  'test-linux-platform',
  'tests/test-linux-platform.c',
  dependencies: linux_platform_dep,
)

test('linux-platform-api', platform_test_exe, timeout: 30)

udev_manager_test_exe = executable(
  'test-udev-manager',
  'tests/test-udev-manager.c',
  dependencies: linux_platform_dep,
)

test('udev-manager-integration', udev_manager_test_exe, timeout: 30)

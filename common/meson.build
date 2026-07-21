subdir('sc')
subdir('tomlc17')
subdir('sha256')

version_h = vcs_tag(
  input: 'version.h.in',
  output: 'version.h',
  command: ['git', 'describe', '--tags', '--always', '--long'],
  fallback: meson.project_version(),
)

util_lib = static_library(
  'util',
  'util.c',
  'util.h',
  include_directories: include_directories('.')
)

log_lib = static_library(
  'log',
  'log.c',
  'log.h',
  include_directories: include_directories('.'),
  link_with: util_lib
)

event_lib = static_library(
  'event',
  'event.c',
  'event.h',
  include_directories: include_directories('.'),
  link_with: [util_lib, log_lib, sc_lib]
)

wayland_ct_lib = static_library(
  'wayland_ct',
  'wayland_ct.c',
  'wayland_ct.h',
  include_directories: include_directories('.'),
  link_with: [util_lib, log_lib, event_lib, sc_lib]
)

xdg_lib = static_library(
  'xdg',
  'xdg.c',
  'xdg.h',
  include_directories: include_directories('.'),
  link_with: [util_lib, log_lib]
)

config_util_lib = static_library(
  'config_util',
  'config_util.c',
  'config_util.h',
  include_directories: include_directories('.'),
  link_with: [sc_lib, util_lib, log_lib, xdg_lib]
)

io_lib = static_library(
  'io',
  'io.c',
  'io.h',
  include_directories: include_directories('.'),
  link_with: [sc_lib, util_lib, log_lib]
)

ipc_ct_lib = static_library(
  'ipc_ct',
  'ipc_ct.c',
  'ipc_ct.h',
  include_directories: include_directories('.'),
  dependencies: json_dep,
  link_with: [sc_lib, util_lib, log_lib, event_lib]
)

json_util_lib = static_library(
  'json_util',
  'json_util.c',
  'json_util.h',
  include_directories: include_directories('.'),
  dependencies: json_dep,
  dependencies: json_dep,
  link_with: [util_lib, log_lib]
)

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
  link_with: [util_lib, log_lib]
)

wayland_ct_lib = static_library(
  'wayland_ct',
  'wayland_ct.c',
  'wayland_ct.h',
  include_directories: include_directories('.'),
  link_with: [util_lib, log_lib, event_lib]
)


bytes_lib = static_library(
  'bytes',
  'bytes.c',
  'bytes.h',
  include_directories: include_directories('.'),
)

subdir('sc')
subdir('tomlc17')

prog_python = find_program('python3')

if prog_python.found()
  db_schema = custom_target(
    'db_schema.h',
    output: 'db_schema.h',
    input: 'db_schema.sql',
    command: [prog_python, meson.current_source_dir() + '/db_schema_gen.py']

  )
endif

swayclip = executable(
  'swayclip',
  'main.c',
  'wayland.c',
  'config.c',
  'database.c',
  'ipc.c',
  db_schema,
  version_h,
  include_directories: [includes, include_directories('.')],
  dependencies: [wl_dep, json_dep, sqlite_dep, xstructs_dep],
  link_with: [
    event_lib,
    log_lib,
    wayland_ct_lib,
    toml_lib,
    xdg_lib,
    config_util_lib,
    daemon_proto_libs,
    sha256_lib,
    io_lib,
    ipc_ct_lib,
    json_util_lib
  ],
  install: true
)

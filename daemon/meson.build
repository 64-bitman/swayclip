swayclip = executable(
  'swayclip',
  'main.c',
  'wayland.c',
  'config.c',
  'database.c',
  version_h,
  include_directories: [includes, include_directories('.')],
  dependencies: [wl_dep, json_dep, sqlite_dep],
  link_with: [
    sc_lib,
    event_lib,
    log_lib,
    util_lib,
    wayland_ct_lib,
    toml_lib,
    xdg_lib,
    config_util_lib,
    daemon_proto_libs,
    sha256_lib,
    io_lib
  ],
  install: true
)

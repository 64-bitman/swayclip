swayclip = executable(
  'swctl',
  'main.c',
  version_h,
  include_directories: [includes, include_directories('.')],
  dependencies: [json_dep, xstructs_dep],
  link_with: [
    log_lib,
    ipc_ct_lib,
    json_util_lib
  ],
  install: true
)

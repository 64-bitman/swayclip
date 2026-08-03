glib_dep = dependency('glib-2.0', version: '>=2.76', required: false)
gio_dep = dependency('gio-2.0', required: false)
gtk4_dep = dependency('gtk4', required: false)

if not glib_dep.found() or not gio_dep.found() or not gtk4_dep.found()
  subdir_done()
endif

swayclip_gtk = executable(
  'swayclip-gtk',
  'main.c',
  'swayclip-application.c',
  'swayclip-connection.c',
  version_h,
  include_directories: [includes, include_directories('.')],
  dependencies: [glib_dep, gio_dep, gtk4_dep, xstructs_dep, json_dep],
  link_with: [ipc_ct_lib, log_lib, json_util_lib],
  install: true
)

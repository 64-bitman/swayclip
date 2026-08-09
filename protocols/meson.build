wayland_scanner = find_program('wayland-scanner', required: true, native: true)

if not wayland_scanner.found()
  subdir_done()
endif

daemon_protocols = ['ext-data-control-v1', 'wlr-foreign-toplevel-management-unstable-v1']
daemon_proto_libs = []

foreach name : daemon_protocols
  xml = name + '.xml'

  client_header = custom_target(
    name + ' client header',
    input: xml,
    output: name + '.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@']
  )
  code = custom_target(
    name + ' code',
    input: xml,
    output: name + '.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@']
  )

  daemon_proto_libs += static_library(
    name.underscorify() + '_proto',
    [client_header, code],
    include_directories: include_directories('.'),
    dependencies: wl_dep
  )
endforeach

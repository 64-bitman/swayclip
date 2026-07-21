wayland_scanner = find_program('wayland-scanner', required: true, native: true)

if not wayland_scanner.found()
  subdir_done()
endif

daemon_protocols = ['ext-data-control-v1']
daemon_proto_sources = []
foreach protocol : daemon_protocols
  name = protocol
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

  daemon_proto_sources += [client_header, code]
endforeach

daemon_proto_lib = static_library(
  'daemon_proto',
  daemon_proto_sources,
  include_directories: include_directories('.'),
  dependencies: wl_dep
)

picker_protocols = [
  'wlr-layer-shell-unstable-v1',
  'cursor-shape-v1',
  'xdg-shell',
  'tablet-v2',
  'fractional-scale-v1',
  'viewporter'
]
picker_proto_sources = []

foreach protocol : picker_protocols
  name = protocol
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

  picker_proto_sources += [client_header, code]
endforeach

picker_proto_lib = static_library(
  'picker_proto',
  picker_proto_sources,
  include_directories: include_directories('.'),
  dependencies: wl_dep
)

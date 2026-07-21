wayland_scanner = find_program('wayland-scanner', required: true, native: true)

if not wayland_scanner.found()
  subdir_done()
endif

daemon_protocols = ['ext-data-control-v1']
picker_protocols = [
  'wlr-layer-shell-unstable-v1',
  'cursor-shape-v1',
  'xdg-shell',
  'tablet-v2',
  'fractional-scale-v1',
  'viewporter'
]
test_protocols = ['ext-transient-seat-v1', 'ext-data-control-v1']

# Union of all protocols, deduplicated
all_protocols = []
foreach p : daemon_protocols + picker_protocols + test_protocols
  if not all_protocols.contains(p)
    all_protocols += p
  endif
endforeach

# One generated library per protocol
proto_libs = {}
foreach name : all_protocols
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
  proto_libs += {name: static_library(
    name.underscorify() + '_proto',
    [client_header, code],
    include_directories: include_directories('.'),
    dependencies: wl_dep
    )}
endforeach

# Helper to turn a protocol-name list into a list of libraries
daemon_proto_libs = []
foreach name : daemon_protocols
  daemon_proto_libs += proto_libs[name]
endforeach

picker_proto_libs = []
foreach name : picker_protocols
  picker_proto_libs += proto_libs[name]
endforeach

test_proto_libs = []
foreach name : test_protocols
  test_proto_libs += proto_libs[name]
endforeach

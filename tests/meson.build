py = import('python')

pytest = py.find_installation('python3', modules: ['pytest'], required: false)

if not pytest.found()
  subdir_done()
endif

wlr_dep = dependency('wlroots-0.20', required: false)
server_dep = dependency('wayland-server', required: false)

if not wlr_dep.found() or not server_dep.found()
  subdir_done()
endif

testserver = executable(
  'testserver',
  'testserver.c',
  c_args: ['-DWLR_USE_UNSTABLE'],
  include_directories: [includes, include_directories('.')],
  dependencies: [server_dep, wlr_dep, json_dep, xstructs_dep]
)

tests = ['clipboard']

foreach test : tests
  test(
    test,
    pytest,
    args: ['-m', 'pytest', meson.current_source_dir(), '-v'],
    workdir: meson.current_source_dir(),
    env: {
      'TEST_DAEMON': swayclip.full_path(),
      'TEST_SERVER': testserver.full_path()
    }
  )
endforeach

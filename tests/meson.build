py = import('python')

pytest = py.find_installation('python3', modules: ['pytest'], required: false)

if not pytest.found()
  subdir_done()
endif

wlr_dep = dependency('wlroots-0.20')

testserver = executable(
  'testserver',
  'testserver.c',
  include_directories: [includes, include_directories('.')],
  dependencies: [wl_dep, json_dep, xstructs_dep],
  link_with: [test_proto_libs]
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

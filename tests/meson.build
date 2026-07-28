glib_dep = dependency('glib-2.0', required: false)
gio_dep = dependency('gio-2.0', required: false)

if not glib_dep.found() or not gio_dep.found()
  subdir_done()
endif

tests = {
  'clipboard': {
    'sources': ['daemon.c', 'compositor.c', 'client.c', 'ipc.c'],
    'deps': [wl_dep, sqlite_dep, json_dep, xstructs_dep],
    'link': [test_proto_libs, ipc_ct_lib]
  }
}

foreach test_name, test_opts : tests
  extra_sources = test_opts.get('sources', []) + ['util.c']
  extra_deps = test_opts.get('deps', [])
  link_with = test_opts.get('link', [])
  sources = ['test_' + test_name + '.c'] + extra_sources
  deps = [glib_dep, gio_dep] + extra_deps

  bin = executable(
    test_name,
    sources,
    include_directories: [include_directories('.'), includes],
    dependencies: deps,
    link_with: link_with
  )

  test(
    test_name,
    bin,
    env: [
      'G_TEST_SRCDIR=@0@'.format(meson.current_source_dir()),
      'G_TEST_BUILDDIR=@0@'.format(meson.current_build_dir()),
      'TEST_BINARY=@0@'.format(swayclip.full_path())
    ],
    protocol: 'tap',
  )
endforeach

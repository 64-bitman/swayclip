project(
  'swayclip', 'c',
  version: '0.1.0',
  license: 'GPL-3.0-or-later',
  meson_version: '>= 0.59.0',
  default_options: [
    'c_std=c11',
    'warning_level=2',
    'b_ndebug=if-release'
  ],
)

add_project_arguments(
  [
    '-D_GNU_SOURCE',
    '-Wcast-align',
    '-Winit-self',
    '-Wnull-dereference',
    '-Wno-float-equal',
  ],
  language: 'c'
)

if get_option('buildtype').startswith('debug')
  add_project_arguments(
    '-DDEBUG',
    '-ggdb3',
    language : 'c'
  )
endif

wl_dep = dependency('wayland-client')
json_dep = dependency('json-c')
sqlite_dep = dependency('sqlite3', version: '>=3.38')

xstructs_dep = subproject('xstructs').get_variable('xstructs_dep')

includes = include_directories('.')

# Thanks sway
scdoc_dep = dependency('scdoc', version: '>=1.9.2', native: true, required: get_option('man-pages'))
if scdoc_dep.found()
	scdoc_prog = find_program(scdoc_dep.get_variable(pkgconfig: 'scdoc'), native: true)
    mandir = get_option('mandir')
    man_files = [
      'docs/swayclip.1.scd',
      'docs/swayclip.5.scd',
      'docs/swctl.1.scd'
    ]

    foreach filename : man_files
      topic = filename.split('.')[-3].split('/')[-1]
      section = filename.split('.')[-2]
      output = '@0@.@1@'.format(topic, section)

      custom_target(
        output,
        input: filename,
        output: output,
        command: scdoc_prog,
        install: true,
        feed: true,
        capture: true,
        install_dir: '@0@/man@1@'.format(mandir, section)
      )
    endforeach
  endif

subdir('protocols')
subdir('common')
subdir('daemon')
if get_option('swctl')
  subdir('cli')
endif
subdir('tests')

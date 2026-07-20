project(
  'swayclip',
  'c',
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
    '-Winline',
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
sqlite_dep = dependency('sqlite3')

includes = include_directories('.')

subdir('protocol')
subdir('common')
subdir('daemon')
subdir('tests')

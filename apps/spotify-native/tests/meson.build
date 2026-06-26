if get_option('tests')
  test_shannon = executable('test-shannon',
    'test_shannon.c',
    '../src/spotify/shannon.c',
    include_directories: [config_inc, include_directories('../src'), include_directories('../src/spotify')],
    dependencies: [glib_dep, gio_dep],
  )
  test('shannon-cipher', test_shannon)
endif

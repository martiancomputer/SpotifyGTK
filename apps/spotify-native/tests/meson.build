if get_option('tests')
  test_shannon = executable('test-shannon',
    'test_shannon.c',
    '../src/spotify/shannon.c',
    include_directories: [config_inc, include_directories('../src'), include_directories('../src/spotify')],
    dependencies: [glib_dep, gio_dep],
  )
  test('shannon-cipher', test_shannon)

  test_protobuf = executable('test-protobuf',
    'test_protobuf.c',
    '../src/spotify/protobuf_min.c',
    include_directories: [config_inc, include_directories('../src')],
    dependencies: [glib_dep, gio_dep],
  )
  test('protobuf-wire-format', test_protobuf)

  test_login_encoding = executable('test-login-encoding',
    'test_login_encoding.c',
    '../src/spotify/protobuf_min.c',
    include_directories: [config_inc, include_directories('../src')],
    dependencies: [glib_dep, gio_dep],
  )
  test('login-message-encoding', test_login_encoding)

  test_dh = executable('test-dh',
    'test_dh.c',
    '../src/spotify/dh.c',
    include_directories: [config_inc, include_directories('../src')],
    dependencies: openssl_dep.found() ? [glib_dep, gio_dep, openssl_dep] : [glib_dep, gio_dep],
  )
  if openssl_dep.found()
    test('dh-key-agreement', test_dh)
  else
    message('Skipping dh-key-agreement test -- built without OpenSSL, dh.c is a no-op')
  endif
endif

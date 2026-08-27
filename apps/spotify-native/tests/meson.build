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

  test_connect_options = executable('test-connect-options',
    'test_connect_options.c',
    '../src/spotify/connect_options.c',
    '../src/spotify/protobuf_min.c',
    include_directories: [config_inc, include_directories('../src')],
    dependencies: [glib_dep, gio_dep],
  )
  test('connect-shuffle-options', test_connect_options)

  test_track_meta = executable('test-track-meta',
    'test_track_meta.c',
    '../src/spotify/protobuf_min.c',
    '../src/spotify/track_meta.c',
    include_directories: [config_inc, include_directories('../src')],
    dependencies: [glib_dep, gio_dep],
  )
  test('track-display-metadata', test_track_meta)

  test_context_uri = executable('test-context-uri',
    'test_context_uri.c',
    '../src/spotify/spclient.c',
    '../src/spotify/protobuf_min.c',
    '../src/spotify/track_meta.c',
    include_directories: [config_inc, include_directories('../src')],
    dependencies: [glib_dep, gio_dep, soup_dep, json_glib_dep],
  )
  test('context-uri-building', test_context_uri)

  test_apresolve = executable('test-apresolve',
    'test_apresolve.c',
    '../src/spotify/apresolve.c',
    include_directories: [config_inc, include_directories('../src')],
    dependencies: [glib_dep, gio_dep, soup_dep, json_glib_dep],
  )
  test('apresolve-parsing', test_apresolve)

  test_dsp = executable('test-dsp',
    'test_dsp.c',
    '../src/audio/dsp.c',
    include_directories: [config_inc, include_directories('../src')],
    dependencies: [glib_dep, m_dep],
  )
  test('dsp-equaliser', test_dsp)

  test_collection = executable('test-collection',
    'test_collection.c',
    '../src/spotify/collection.c',
    '../src/spotify/protobuf_min.c',
    # The parser turns a raw 16-byte GID into a track URI, which is where the
    # base62 encoder lives.
    '../src/spotify/track_meta.c',
    include_directories: [config_inc, include_directories('../src')],
    dependencies: [glib_dep, gio_dep],
  )
  test('collection-write-encoding', test_collection)

  test_resampler = executable('test-resampler',
    'test_resampler.c',
    '../src/audio/resampler.c',
    include_directories: [config_inc, include_directories('../src')],
    dependencies: [glib_dep, m_dep],
  )
  test('resampler', test_resampler)

  test_login_encoding = executable('test-login-encoding',
    'test_login_encoding.c',
    '../src/spotify/protobuf_min.c',
    include_directories: [config_inc, include_directories('../src')],
    dependencies: [glib_dep, gio_dep],
  )
  test('login-message-encoding', test_login_encoding)

  test_native_auth = executable('test-native-auth',
    'test_native_auth.c',
    '../src/spotify/native_auth.c',
    include_directories: [config_inc, include_directories('../src')],
    dependencies: [glib_dep, gio_dep, soup_dep, json_glib_dep],
  )
  test('native-auth-constants', test_native_auth)

  test_streaming_auth = executable('test-streaming-auth',
    'test_streaming_auth.c',
    '../src/spotify/protobuf_min.c',
    include_directories: [config_inc, include_directories('../src')],
    dependencies: [glib_dep, gio_dep],
  )
  test('streaming-auth-encoding', test_streaming_auth)

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

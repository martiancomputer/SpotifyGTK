if get_option('tests')
  if enable_gui
    test_track_row_pointer = executable('test-track-row-pointer',
      'test_track_row_pointer.c',
      include_directories: [config_inc, include_directories('../src')],
      dependencies: [gtk4_dep, adw_check, json_glib_dep, m_dep],
    )
    test('track-row-pointer-routing', test_track_row_pointer, timeout: 10)
    test_smooth_scroll = executable('test-smooth-scroll',
      'test_smooth_scroll.c', '../src/ui/settings.c',
      include_directories: [config_inc, include_directories('../src')],
      dependencies: [gtk4_dep, gio_dep, m_dep],
    )
    test('smooth-scroll-coordinates', test_smooth_scroll, timeout: 10)
    test_page_stack = executable('test-page-stack',
      'test_page_stack.c', '../src/ui/page_stack.c',
      include_directories: [include_directories('../src')],
      dependencies: [gtk4_dep],
    )
    test('page-stack-visibility', test_page_stack, timeout: 10)
  endif
  test_ap_framing = executable('test-ap-framing',
    'test_ap_framing.c',
    '../src/spotify/apresolve.c', '../src/spotify/dh.c',
    '../src/spotify/handshake_crypto.c', '../src/spotify/protobuf_min.c',
    '../src/spotify/shannon.c', '../src/log_file.c',
    include_directories: [config_inc, include_directories('../src')],
    dependencies: openssl_dep.found() ? [glib_dep, gio_dep, soup_dep, json_glib_dep, openssl_dep] : [glib_dep, gio_dep, soup_dep, json_glib_dep],
  )
  test('ap-packet-framing', test_ap_framing, timeout: 10)

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

  test_settings = executable('test-settings',
    'test_settings.c',
    '../src/ui/settings.c',
    include_directories: [config_inc, include_directories('../src')],
    dependencies: [glib_dep, gio_dep],
  )
  test('settings', test_settings)

  if have_local_av
    test_local_catalog = executable('test-local-catalog',
      'test_local_catalog.c',
      '../src/ui/local_catalog.c',
      '../src/ui/settings.c',
      include_directories: [config_inc, include_directories('../src')],
      dependencies: [glib_dep, gio_dep, soup_dep, json_glib_dep,
                     avformat_dep, avcodec_dep,
                     avutil_dep, swresample_dep],
    )
    test('local-catalog', test_local_catalog)

    test_local_playback = executable('test-local-playback',
      'test_local_playback.c',
      '../src/audio/local_playback.c',
      include_directories: [config_inc, include_directories('../src')],
      dependencies: [glib_dep, gio_dep, avformat_dep, avcodec_dep,
                     avutil_dep, swresample_dep],
    )
    test('local-playback', test_local_playback)
  endif

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
    '../src/log_file.c',
    include_directories: [config_inc, include_directories('../src')],
    dependencies: [glib_dep, gio_dep, soup_dep, json_glib_dep],
  )
  test('context-uri-building', test_context_uri)

  test_apresolve = executable('test-apresolve',
    'test_apresolve.c',
    '../src/spotify/apresolve.c',
    '../src/log_file.c',
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

  test_local_favorites = executable('test-local-favorites',
    'test_local_favorites.c', '../src/ui/local_favorites.c',
    include_directories: [config_inc, include_directories('../src')],
    dependencies: [glib_dep, gio_dep, json_glib_dep, soup_dep],
  )
  test('local-favorites', test_local_favorites)

  test_device_playlists = executable('test-device-playlists',
    'test_device_playlists.c', '../src/ui/device_playlists.c',
    include_directories: [config_inc, include_directories('../src')],
    dependencies: [glib_dep, gio_dep, json_glib_dep, soup_dep],
  )
  test('device-playlists', test_device_playlists)

  test_resampler = executable('test-resampler',
    'test_resampler.c',
    '../src/audio/resampler.c',
    include_directories: [config_inc, include_directories('../src')],
    dependencies: [glib_dep, m_dep],
  )
  test('resampler', test_resampler)

  test_output_format = executable('test-output-format',
    'test_output_format.c',
    '../src/audio/output.c',
    include_directories: [config_inc, include_directories('../src')],
    dependencies: [glib_dep],
  )
  test('output-format', test_output_format)

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
    '../src/log_file.c',
    include_directories: [config_inc, include_directories('../src')],
    dependencies: [glib_dep, gio_dep, soup_dep, json_glib_dep],
  )
  test('native-auth-constants', test_native_auth)

  test_streaming_auth = executable('test-streaming-auth',
    'test_streaming_auth.c',
    '../src/spotify/protobuf_min.c',
    '../src/spotify/cdn.c',
    '../src/log_file.c',
    include_directories: [config_inc, include_directories('../src')],
    dependencies: openssl_dep.found() ? [glib_dep, gio_dep, soup_dep, openssl_dep] : [glib_dep, gio_dep, soup_dep],
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

if get_option('tests')
  test_deps = [glib_dep, gio_dep, json_glib_dep]
  if libsecret_dep.found()
    test_deps += libsecret_dep
  endif

  test_auth = executable('test-auth',
    'test_auth.c',
    '../src/auth.c',
    include_directories: [config_inc, include_directories('../src')],
    dependencies: [test_deps, soup_dep],
  )
  test('auth-pkce', test_auth)

  test_api = executable('test-api',
    'test_api.c',
    '../src/auth.c',
    '../src/api.c',
    include_directories: [config_inc, include_directories('../src')],
    dependencies: [test_deps, soup_dep],
  )
  test('api-requests', test_api)

  test_shannon = executable('test-shannon',
    'test_shannon.c',
    '../src/spotify/shannon.c',
    include_directories: [config_inc, include_directories('../src'), include_directories('../src/spotify')],
    dependencies: [test_deps],
  )
  test('shannon-cipher', test_shannon)

  test_image_cache = executable('test-image-cache',
    'test_image_cache.c',
    '../src/image_cache.c',
    include_directories: [config_inc, include_directories('../src')],
    dependencies: [all_deps],
  )
  test('image-cache', test_image_cache)
endif

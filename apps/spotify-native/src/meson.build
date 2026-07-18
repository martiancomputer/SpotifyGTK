subdir('spotify')
subdir('audio')

executable('spotify-native-harness',
  'main.c',
  spotify_proto_sources,
  audio_engine_sources,
  include_directories: [config_inc, include_directories('.')],
  dependencies: all_deps,
  # The GTK shell supervises this executable as its reliable first playback
  # bridge, so install it beside spotify-native as well as keeping it useful
  # from a development build directory.
  install: true,
)

if enable_gui
  spotify_connect_api_sources = files(
    '../../spotify-connect/src/auth.c',
    '../../spotify-connect/src/api.c',
  )
  spotify_connect_inc = include_directories('../../spotify-connect/src')
  executable('spotify-native',
    ['gui_main.c', 'player_service.c', 'main.c'] + spotify_connect_api_sources + spotify_proto_sources + audio_engine_sources,
    include_directories: [config_inc, include_directories('.'), spotify_connect_inc],
    dependencies: all_deps + [gtk4_dep],
    c_args: ['-DSPOTIFYGTK_ENGINE_LIBRARY'],
    install: true,
  )
endif

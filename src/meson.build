subdir('spotify')
subdir('audio')

spotifygtk_sources = files(
  'main.c',
  'app.c',
  'auth.c',
  'api.c',
  'player.c',
  'image_cache.c',
  'ui/window.c',
  'ui/now_playing.c',
  'ui/search_page.c',
)

executable('spotifygtk',
  spotifygtk_sources,
  spotify_proto_sources,
  audio_engine_sources,
  include_directories: [config_inc, include_directories('.')],
  dependencies: all_deps,
  install: true,
)

spotify_connect_sources = files(
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

executable('spotify-connect',
  spotify_connect_sources,
  include_directories: [config_inc, include_directories('.')],
  dependencies: all_deps,
  install: true,
)

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

# Small standalone tool, reuses auth.c's real token storage/lookup
# logic rather than reimplementing it, to extract a stored OAuth
# token for use elsewhere (e.g. testing apps/spotify-native's login
# against a real account). Not installed -- a dev/debug utility, not
# part of the shipped app.
executable('print-token',
  ['../tools/print-token.c', 'auth.c'],
  include_directories: [config_inc, include_directories('.')],
  dependencies: all_deps,
  install: false,
)

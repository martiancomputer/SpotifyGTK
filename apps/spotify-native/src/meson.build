subdir('spotify')
subdir('audio')

executable('spotify-native-harness',
  'main.c',
  spotify_proto_sources,
  audio_engine_sources,
  include_directories: [config_inc, include_directories('.')],
  dependencies: all_deps,
  install: false,
)

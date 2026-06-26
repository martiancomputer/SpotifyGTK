desktop_file = configure_file(
  input:  'com.github.spotifygtk.SpotifyConnect.desktop.in',
  output: 'com.github.spotifygtk.SpotifyConnect.desktop',
  configuration: conf,
)
install_data(desktop_file,
  install_dir: join_paths(get_option('datadir'), 'applications'))

install_data('com.github.spotifygtk.SpotifyConnect.gschema.xml',
  install_dir: join_paths(get_option('datadir'), 'glib-2.0', 'schemas'))

gnome = import('gnome')
gnome.compile_schemas(depend_files: 'com.github.spotifygtk.SpotifyConnect.gschema.xml')

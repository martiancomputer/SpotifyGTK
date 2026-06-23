desktop_file = configure_file(
  input:  'com.github.spotifygtk.SpotifyGTK.desktop.in',
  output: 'com.github.spotifygtk.SpotifyGTK.desktop',
  configuration: conf,
)
install_data(desktop_file,
  install_dir: join_paths(get_option('datadir'), 'applications'))

install_data('com.github.spotifygtk.SpotifyGTK.gschema.xml',
  install_dir: join_paths(get_option('datadir'), 'glib-2.0', 'schemas'))

gnome = import('gnome')
gnome.compile_schemas(depend_files: 'com.github.spotifygtk.SpotifyGTK.gschema.xml')

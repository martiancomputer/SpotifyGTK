# Desktop integration for spotify-native.
#
# Without these the binary installs but the app does not exist as far as a
# desktop is concerned: no launcher entry, no icon, nothing for a software
# centre to describe. Both the deb and the AppImage need all three, so they
# live here rather than in either packaging script.

desktop_file = configure_file(
  input:  'com.github.spotifygtk.SpotifyNative.desktop.in',
  output: 'com.github.spotifygtk.SpotifyNative.desktop',
  configuration: conf,
)
install_data(desktop_file,
  install_dir: join_paths(get_option('datadir'), 'applications'))

install_data('com.github.spotifygtk.SpotifyNative.metainfo.xml',
  install_dir: join_paths(get_option('datadir'), 'metainfo'))

# Scalable rather than a pile of rendered sizes: one SVG covers every scale a
# HiDPI desktop asks for.
install_data('com.github.spotifygtk.SpotifyNative.svg',
  install_dir: join_paths(get_option('datadir'), 'icons', 'hicolor',
                          'scalable', 'apps'))

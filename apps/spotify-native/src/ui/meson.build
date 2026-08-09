gnome = import('gnome')

# Icons the theme does not provide. Compiled in rather than installed to a
# theme directory so they resolve from a build tree as well as an install.
ui_resources = gnome.compile_resources(
  'spotifygtk-ui-resources', 'icons.gresource.xml',
  source_dir: meson.current_source_dir(),
  c_name: 'spotifygtk_ui_resources',
)

# UI module for spotify-native - matching mockup design

ui_sources = ui_resources + files(
  'context_menu.c',
  'window.c',
  'sidebar.c',
  'playback_bar.c',
  'now_playing_panel.c',
  'settings.c',
  'settings_page.c',
  'cover_loader.c',
  'track_item.c',
  'track_row.c',
  'track_list.c',
  'eq_graph.c',
  'smooth_scroll.c',
  'album_grid.c',
  'home_page.c',
  'search_page.c',
  'liked_songs_page.c',
  'library_page.c',
  'context_page.c',
  'artist_page.c',
)

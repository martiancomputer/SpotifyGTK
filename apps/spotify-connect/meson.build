project('spotify-connect', 'c',
  version: '0.1.0',
  meson_version: '>= 1.0.0',
  default_options: [
    'warning_level=2',
    'werror=false',
    'c_std=gnu17',
  ]
)

app_id = 'com.github.spotifygtk.SpotifyConnect'

# ── Profile resolution ───────────────────────────────────────────────────────
# Two tracks, one codebase. See build-profiles/{stable,nightly}.ini
# Note: this app does no audio playback of its own (it controls an
# existing Spotify Connect device via the Web API), so the profile
# split here only affects GTK version + image-decode acceleration --
# audio backend selection lives entirely in ../spotify-native/.
profile      = get_option('profile')
is_nightly   = (profile == 'nightly')

enable_vaapi  = get_option('enable_vaapi')
enable_dmabuf = get_option('enable_dmabuf')

# ── Core dependencies ────────────────────────────────────────────────────────
gtk4_min = is_nightly ? '>= 4.14' : '>= 4.0'

gtk4_dep       = dependency('gtk4',          version: gtk4_min)
adw_dep        = dependency('libadwaita-1',  version: '>= 1.4')
soup_dep       = dependency('libsoup-3.0',   version: '>= 3.4')
json_glib_dep  = dependency('json-glib-1.0', version: '>= 1.6')
glib_dep       = dependency('glib-2.0',      version: '>= 2.76')
gio_dep        = dependency('gio-2.0',       version: '>= 2.76')

# Token storage -- optional, falls back to a chmod 600 file in ~/.config.
libsecret_dep  = dependency('libsecret-1',   version: '>= 0.20', required: false)

cc    = meson.get_compiler('c')
m_dep = cc.find_library('m', required: false)

# JPEG decode for album art -- libjpeg-turbo ships as a drop-in libjpeg
# replacement on every major distro, so we look for the plain library
# name + header rather than assuming a turbo-specific pkg-config file.
jpeg_lib           = cc.find_library('jpeg', required: false)
have_libjpeg_turbo = jpeg_lib.found() and cc.has_header('jpeglib.h')

all_deps = [gtk4_dep, adw_dep, soup_dep, json_glib_dep, glib_dep, gio_dep]
if libsecret_dep.found()
  all_deps += libsecret_dep
endif
if m_dep.found()
  all_deps += m_dep
endif
if have_libjpeg_turbo
  all_deps += jpeg_lib
endif

# ── Optional hardware acceleration for the image cache ──────────────────────
have_vaapi = false
if enable_vaapi
  libva_dep     = dependency('libva',     version: '>= 2.0', required: false)
  libva_drm_dep = dependency('libva-drm', required: false)
  if libva_dep.found() and libva_drm_dep.found()
    all_deps += [libva_dep, libva_drm_dep]
    have_vaapi = true
  endif
endif

have_dmabuf = enable_dmabuf and is_nightly and gtk4_dep.version().version_compare('>= 4.14')

# ── Compiler flags ────────────────────────────────────────────────────────────
add_project_arguments(
  cc.get_supported_arguments([
    '-Wno-unused-parameter',
    '-Wno-missing-field-initializers',
    '-fno-strict-aliasing',
  ]),
  language: 'c'
)

# ── Config header ─────────────────────────────────────────────────────────────
conf = configuration_data()
conf.set_quoted('APP_ID',      app_id)
conf.set_quoted('APP_VERSION', meson.project_version())
conf.set_quoted('APP_PROFILE', profile)
conf.set_quoted('GETTEXT_PACKAGE', meson.project_name())
conf.set10('SPOTIFYGTK_NIGHTLY',  is_nightly)
conf.set10('HAVE_LIBSECRET',      libsecret_dep.found())
conf.set10('HAVE_LIBJPEG_TURBO',  have_libjpeg_turbo)
conf.set10('HAVE_VAAPI',          have_vaapi)
conf.set10('HAVE_DMABUF',         have_dmabuf)
configure_file(output: 'config.h', configuration: conf)
config_inc = include_directories('.')

# ── Subdirectories ────────────────────────────────────────────────────────────
subdir('src')
subdir('data')
subdir('po')
subdir('tests')

# ── Summary ───────────────────────────────────────────────────────────────────
summary({
  'Profile'     : profile,
  'GTK4'        : gtk4_dep.version(),
  'Libadwaita'  : adw_dep.version(),
}, section: 'Build')

summary({
  'VA-API (hw JPEG decode)'   : have_vaapi,
  'libjpeg-turbo (SIMD JPEG)' : have_libjpeg_turbo,
  'GdkDmabufTexture'          : have_dmabuf,
  'libsecret (token store)'   : libsecret_dep.found(),
}, section: 'Optional acceleration')

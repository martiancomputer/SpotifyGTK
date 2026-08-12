project('spotify-native', 'c',
  version: '0.1.0',
  meson_version: '>= 1.0.0',
  default_options: [
    'warning_level=2',
    'werror=false',
    'c_std=gnu17',
  ]
)

# ── Profile resolution ───────────────────────────────────────────────────────
# No GTK dependency here -- this app has no UI yet, it's the standalone
# protocol + audio engine. The stable/nightly split here is purely about
# audio output backend choice. See build-profiles/{stable,nightly}.ini
profile         = get_option('profile')
is_nightly      = (profile == 'nightly')
enable_pipewire = get_option('enable_pipewire')
enable_gui      = get_option('enable_gui')

# Diagnostics that were previously added to chase a bug and then stripped out
# again, which meant the next bug started from nothing. They stay in the source
# and compile away unless asked for.
if get_option('verbose_logging')
  add_project_arguments('-DSPOTIFYGTK_VERBOSE', language: 'c')
endif

# ── Core dependencies ────────────────────────────────────────────────────────
glib_dep       = dependency('glib-2.0',      version: '>= 2.76')
gio_dep        = dependency('gio-2.0',       version: '>= 2.76')
soup_dep       = dependency('libsoup-3.0',   version: '>= 3.4')  # cdn.c HTTPS Range fetch, native_auth.c OAuth
json_glib_dep  = dependency('json-glib-1.0', version: '>= 1.6')  # native_auth.c token response parsing
gtk4_dep       = dependency('gtk4', version: '>= 4.0', required: enable_gui)

# The UI is laid out against a specific GTK, not merely a minimum one.
#
# Widget metrics changed enough across 4.14 -> 4.22 that the same source
# produces a visibly different window: the Now Playing cover stops filling its
# panel, and the playback and volume bars take different sizes. An AppImage
# built on Ubuntu 24.04 (GTK 4.14) showed exactly that, and the fix is not to
# scatter version-dependent workarounds through the UI.
#
# So this is a hard floor rather than a warning. -Dallow_old_gtk=true builds
# anyway, for anyone who would rather have a working client that looks a little
# off than none at all.
gtk_designed_for = '4.22.4'
adw_designed_for = '1.9.0'

if gtk4_dep.found() and not get_option('allow_old_gtk')
  if gtk4_dep.version().version_compare('< ' + gtk_designed_for)
    error(
      'GTK ' + gtk4_dep.version() + ' is older than ' + gtk_designed_for + ', which the UI is laid out against.\n' +
      '  The build would succeed and the window would look wrong -- cover art, the playback bar\n' +
      '  and spacing all differ on older GTK.\n' +
      '  Build anyway with:  meson setup build -Dallow_old_gtk=true'
    )
  endif

  # Checked alongside GTK because libadwaita draws much of what differs --
  # entries, popovers, the header bar -- so a new GTK with an old libadwaita
  # lands in the same place.
  adw_check = dependency('libadwaita-1', version: '>= 1.4', required: false)
  if adw_check.found() and adw_check.version().version_compare('< ' + adw_designed_for)
    error(
      'libadwaita ' + adw_check.version() + ' is older than ' + adw_designed_for + ', which the UI is laid out against.\n' +
      '  Build anyway with:  meson setup build -Dallow_old_gtk=true'
    )
  endif
endif

# Audio decode -- libogg/libvorbis are 20+ year old, ABI-frozen, pure C.
ogg_dep        = dependency('ogg',           version: '>= 1.3')
vorbis_dep     = dependency('vorbis',        version: '>= 1.3')
vorbisfile_dep = dependency('vorbisfile',    version: '>= 1.3')

# Crypto -- AES-CTR for CDN audio decrypt (cdn.c). The Shannon cipher
# itself (AP transport encryption) is hand-rolled, no library needed.
openssl_dep    = dependency('openssl',       version: '>= 3.0', required: false)

all_deps = [
  glib_dep, gio_dep, soup_dep, json_glib_dep,
  ogg_dep, vorbis_dep, vorbisfile_dep,
]
if openssl_dep.found()
  all_deps += openssl_dep
endif

# ── Optional audio output backends (runtime-probed regardless) ─────────────
have_pipewire = false
if enable_pipewire
  pipewire_dep = dependency('libpipewire-0.3', required: false)
  if pipewire_dep.found()
    all_deps += pipewire_dep
    have_pipewire = true
  endif
endif

is_windows = host_machine.system() == 'windows'

pulse_dep        = dependency('libpulse',        required: false)
pulse_simple_dep = dependency('libpulse-simple', required: false)
alsa_dep         = dependency('alsa',            required: false)
have_pulse       = pulse_dep.found() and pulse_simple_dep.found()
if have_pulse
  all_deps += [pulse_dep, pulse_simple_dep]
endif
if alsa_dep.found()
  all_deps += alsa_dep
endif

# ── Compiler flags ────────────────────────────────────────────────────────────
cc = meson.get_compiler('c')

# libm: the now-playing equaliser animates its bar heights with sin().
m_dep = cc.find_library('m', required: false)

# WASAPI is COM, so it needs ole32; nothing else Windows-specific is linked.
# This has to live below cc, not up with the other dependency lookups -- the
# earlier placement referenced cc before meson.get_compiler() defined it, which
# only ever failed on Windows because the block sits behind `if is_windows` and
# never evaluated on Linux.
if is_windows
  all_deps += cc.find_library('ole32', required: true)
endif
if m_dep.found()
  all_deps += m_dep
endif
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
conf.set_quoted('APP_VERSION', meson.project_version())
conf.set_quoted('APP_ID', 'com.github.spotifygtk.SpotifyNative')
# For running uninstalled: the icon lives in the source tree rather than in an
# installed theme, so a build-dir run needs to be told where to look.
conf.set_quoted('SPOTIFYGTK_SRC_DATA_DIR',
                join_paths(meson.current_source_dir(), 'data'))
conf.set_quoted('APP_PROFILE', profile)
conf.set10('SPOTIFY_NATIVE_NIGHTLY', is_nightly)
conf.set10('HAVE_OPENSSL',  openssl_dep.found())
conf.set10('HAVE_PIPEWIRE', have_pipewire)
conf.set10('HAVE_PULSE',    have_pulse)
conf.set10('HAVE_ALSA',     alsa_dep.found())
conf.set10('HAVE_WASAPI',   is_windows)
configure_file(output: 'config.h', configuration: conf)
config_inc = include_directories('.')

# ── Subdirectories ────────────────────────────────────────────────────────────
subdir('data')
subdir('src')
subdir('tests')

# ── Summary ───────────────────────────────────────────────────────────────────
summary({
  'Profile'     : profile,
}, section: 'Build')

summary({
  'PipeWire (native audio)' : have_pipewire,
  'PulseAudio'              : have_pulse,
  'ALSA'                    : alsa_dep.found(),
  'OpenSSL (CDN decrypt)'   : openssl_dep.found(),
  'GTK4 native shell'       : gtk4_dep.found(),
}, section: 'Audio backends')

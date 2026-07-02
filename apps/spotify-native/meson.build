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

# ── Core dependencies ────────────────────────────────────────────────────────
glib_dep       = dependency('glib-2.0',      version: '>= 2.76')
gio_dep        = dependency('gio-2.0',       version: '>= 2.76')
soup_dep       = dependency('libsoup-3.0',   version: '>= 3.4')  # cdn.c HTTPS Range fetch, native_auth.c OAuth
json_glib_dep  = dependency('json-glib-1.0', version: '>= 1.6')  # native_auth.c token response parsing

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
conf.set_quoted('APP_PROFILE', profile)
conf.set10('SPOTIFY_NATIVE_NIGHTLY', is_nightly)
conf.set10('HAVE_OPENSSL',  openssl_dep.found())
conf.set10('HAVE_PIPEWIRE', have_pipewire)
conf.set10('HAVE_PULSE',    have_pulse)
conf.set10('HAVE_ALSA',     alsa_dep.found())
configure_file(output: 'config.h', configuration: conf)
config_inc = include_directories('.')

# ── Subdirectories ────────────────────────────────────────────────────────────
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
}, section: 'Audio backends')

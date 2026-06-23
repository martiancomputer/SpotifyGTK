project('spotifygtk', 'c',
  version: '0.1.0',
  meson_version: '>= 1.0.0',
  default_options: [
    'warning_level=2',
    'werror=false',
    'c_std=gnu17',
  ]
)

app_id = 'com.github.spotifygtk.SpotifyGTK'

# ── Profile resolution ───────────────────────────────────────────────────────
# Two tracks, one codebase. See build-profiles/{stable,nightly}.ini
profile         = get_option('profile')
is_nightly      = (profile == 'nightly')

enable_vaapi    = get_option('enable_vaapi')
enable_pipewire = get_option('enable_pipewire')
enable_dmabuf   = get_option('enable_dmabuf')
audio_backend   = get_option('audio_backend')

# ── Core dependencies (required on both tracks) ─────────────────────────────
# Minimum versions differ by profile — nightly demands newer GTK for
# GdkDmabufTexture; stable stays on the 4.0 ABI that's been frozen since 2022.
gtk4_min = is_nightly ? '>= 4.14' : '>= 4.0'

gtk4_dep       = dependency('gtk4',          version: gtk4_min)
adw_dep        = dependency('libadwaita-1',  version: '>= 1.4')
soup_dep       = dependency('libsoup-3.0',   version: '>= 3.4')
json_glib_dep  = dependency('json-glib-1.0', version: '>= 1.6')
glib_dep       = dependency('glib-2.0',      version: '>= 2.76')
gio_dep        = dependency('gio-2.0',       version: '>= 2.76')

# Audio decode — libogg/libvorbis are 20+ year old, ABI-frozen, pure C.
ogg_dep        = dependency('ogg',           version: '>= 1.3')
vorbis_dep     = dependency('vorbis',        version: '>= 1.3')
vorbisfile_dep = dependency('vorbisfile',    version: '>= 1.3')

# Crypto for the Spotify protocol layer (Shannon cipher needs AES/SHA1 for
# the handshake; the stream cipher itself is hand-rolled, no library needed).
openssl_dep    = dependency('openssl',       version: '>= 3.0', required: false)

# Token storage — optional, falls back to a chmod 600 file in ~/.config.
libsecret_dep  = dependency('libsecret-1',   version: '>= 0.20', required: false)

cc = meson.get_compiler('c')

# JPEG decode for album art — libjpeg-turbo ships as a drop-in libjpeg
# replacement on every major distro, so we look for the plain library
# name + header rather than assuming a turbo-specific pkg-config file.
jpeg_lib           = cc.find_library('jpeg', required: false)
have_libjpeg_turbo = jpeg_lib.found() and cc.has_header('jpeglib.h')

all_deps = [
  gtk4_dep, adw_dep, soup_dep, json_glib_dep, glib_dep, gio_dep,
  ogg_dep, vorbis_dep, vorbisfile_dep,
]
if libsecret_dep.found()
  all_deps += libsecret_dep
endif
if openssl_dep.found()
  all_deps += openssl_dep
endif
if have_libjpeg_turbo
  all_deps += jpeg_lib
endif

# ── Optional hardware acceleration (runtime-probed regardless) ──────────────
have_vaapi = false
if enable_vaapi
  libva_dep = dependency('libva', version: '>= 2.0', required: false)
  libva_drm_dep = dependency('libva-drm', required: false)
  if libva_dep.found() and libva_drm_dep.found()
    all_deps += [libva_dep, libva_drm_dep]
    have_vaapi = true
  endif
endif

have_pipewire = false
if enable_pipewire
  pipewire_dep = dependency('libpipewire-0.3', required: false)
  if pipewire_dep.found()
    all_deps += pipewire_dep
    have_pipewire = true
  endif
endif

# PulseAudio / ALSA — always available as candidates regardless of profile,
# since they're the universal fallback even on a nightly/PipeWire system
# (PipeWire ships a pulse-compatible socket anyway).
pulse_dep = dependency('libpulse',  required: false)
alsa_dep  = dependency('alsa',      required: false)
if pulse_dep.found()
  all_deps += pulse_dep
endif
if alsa_dep.found()
  all_deps += alsa_dep
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
conf.set10('SPOTIFYGTK_NIGHTLY',      is_nightly)
conf.set10('HAVE_LIBSECRET',          libsecret_dep.found())
conf.set10('HAVE_OPENSSL',            openssl_dep.found())
conf.set10('HAVE_LIBJPEG_TURBO',      have_libjpeg_turbo)
conf.set10('HAVE_VAAPI',              have_vaapi)
conf.set10('HAVE_PIPEWIRE',           have_pipewire)
conf.set10('HAVE_PULSE',              pulse_dep.found())
conf.set10('HAVE_ALSA',               alsa_dep.found())
conf.set10('HAVE_DMABUF',             have_dmabuf)
conf.set_quoted('DEFAULT_AUDIO_BACKEND', audio_backend)
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
  'VA-API (hw JPEG decode)' : have_vaapi,
  'libjpeg-turbo (SIMD JPEG)' : have_libjpeg_turbo,
  'PipeWire (native audio)' : have_pipewire,
  'PulseAudio'              : pulse_dep.found(),
  'ALSA'                    : alsa_dep.found(),
  'GdkDmabufTexture'        : have_dmabuf,
  'libsecret (token store)' : libsecret_dep.found(),
}, section: 'Optional acceleration')

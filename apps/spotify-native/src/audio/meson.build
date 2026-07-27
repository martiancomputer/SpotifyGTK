audio_engine_sources = files(
  'dsp.c',
  'resampler.c',
  'decoder.c',
  'output.c',
)

# One backend family per platform: the POSIX sound servers do not exist on
# Windows, and WASAPI does not exist anywhere else, so building both would only
# produce link errors for the absent one.
if host_machine.system() == 'windows'
  audio_engine_sources += files('output_wasapi.c')
else
  audio_engine_sources += files('output_pulse.c', 'output_alsa.c')
  if have_pipewire
    audio_engine_sources += files('output_pipewire.c')
  endif
endif

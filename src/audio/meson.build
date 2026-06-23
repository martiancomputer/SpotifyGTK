audio_engine_sources = files(
  'decoder.c',
  'output.c',
  'output_pulse.c',
  'output_alsa.c',
)

if have_pipewire
  audio_engine_sources += files('output_pipewire.c')
endif

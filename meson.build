project('audio_link', ['c', 'cpp'],
  version : '0.1',
  license : 'GPL-3.0-or-later',
  meson_version : '>= 0.40.0',
  default_options : [
    'warning_level=1',
    'buildtype=debugoptimized',
  ]
)

gst_dep = dependency('gstreamer-1.0')
gst_rtp_dep = dependency('gstreamer-rtp-1.0')

executable('audio-link',
  [ 'main.c' ],
  install: true,
  dependencies : [gst_dep, gst_rtp_dep],
)

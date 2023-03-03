project('screencast-pipewire-example', 'c')

libportal = dependency('libportal')
pipewire = dependency('libpipewire-0.3')


executable(
  'example',
  'example.c',
  c_args: [
    # using ".substring(2)" to extract the libportal version,
    # hopefully the version is of the format "M.m" :)
    '-DLIBPORTAL_VERSION=' + libportal.version().substring(2),
  ],
  dependencies: [
    libportal,
    pipewire,
  ],
)

project('os-a11y', 'c', version: '0.1', default_options: ['warning_level=2', 'c_std=gnu17'])
# What has focus in other apps, from the accessibility bus (AT-SPI), for the desktop's screen reader
atspi = dependency('atspi-2')
gio = dependency('gio-2.0')
executable('os-a11y', 'os-a11y.c', dependencies: [atspi, gio], install: true,
           install_dir: get_option('libexecdir'))

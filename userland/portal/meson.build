project('os-portal', 'c', version: '0.1', default_options: ['warning_level=2', 'c_std=gnu17'])
gio = dependency('gio-unix-2.0')
json = dependency('json-glib-1.0')
executable('os-portal', 'os-portal.c', dependencies: [gio, json], install: true,
           install_dir: get_option('libexecdir'))
# How xdg-desktop-portal finds it, and that it is the backend for this desktop
install_data('os.portal', install_dir: get_option('datadir') / 'xdg-desktop-portal' / 'portals')
install_data('os-portals.conf', install_dir: get_option('datadir') / 'xdg-desktop-portal')
install_data('org.freedesktop.impl.portal.desktop.os.service', install_dir: get_option('datadir') / 'dbus-1' / 'services')
